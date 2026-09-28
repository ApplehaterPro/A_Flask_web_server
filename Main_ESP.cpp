/*
 * Project: Warehouse Hazardous Gas & Temperature Monitoring System (Client Node)
 * Target Board: ESP32 (e.g. ESP32 Dev Module, NodeMCU-32S, ESP-WROOM-32)
 *
 * Hardware Modules:
 *   1. MQ-135 Gas / Air Quality Sensor Module
 *      - Detects NH3, NOx, Alcohol, Benzene, Smoke, CO2
 *      - Connected to: ESP32 ADC1 (GPIO 34)
 *   2. DS18B20 Waterproof Temperature Sensor Probe
 *      - 1-Wire Digital Bus (-55°C to +125°C)
 *      - Connected to: GPIO 4 (Requires 4.7kΩ pull-up resistor to 3.3V)
 *   3. 5V Single-Channel Relay Module
 *      - Controls external exhaust fan, siren, or fire alarm
 *      - Connected to: GPIO 26 (Configurable Active-LOW / Active-HIGH)
 *
 * Networking:
 *   - Connects to Wi-Fi and sends live telemetry via HTTP POST (JSON)
 *     to an external Flask server running on another ESP / host.
 *
 *  Module          | Pin on Module        | ESP32 Pin  | Notes
 *-----------------|----------------------|------------|--------------------------------------------------------
 *  MQ-135          | VCC                  | 5V / VIN   | Internal heater requires 5V
 *                  | GND                  | GND        | Common Ground
 *                  | AOUT / AO            | GPIO 34    | ADC1 channel (safe with Wi-Fi)
 *  DS18B20         | Red (VCC)            | 3.3V       | Can also use 5V
 *                  | Black (GND)          | GND        | Common Ground
 *                  | Yellow / Blue (DATA) | GPIO 4     | Connect a 4.7kΩ pull-up resistor between DATA and 3.3V
 *  5V Relay Module | VCC                  | 5V / VIN   | Relay coil requires 5V
 *                  | GND                  | GND        | Common Ground
 *                  | IN                   | GPIO 26    | Output control signal (Active-LOW)
 *                  | COM / NO             | Load       | Connected to your exhaust fan, siren, or safety system
 *
 * Author: Jaivir Singh; Email: jaivirgrewal17@gmail.com
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ==============================================================================
// 1. PIN DEFINITIONS (ESP32)
// ==============================================================================
// MQ-135 Gas Sensor Analog Output:
// GPIO 34 belongs to ADC1. ADC1 pins (GPIO 32-39) remain fully operational
// while Wi-Fi is active (ADC2 pins conflict with Wi-Fi).
#define MQ135_ANALOG_PIN        34

// DS18B20 1-Wire Digital Temperature Sensor Pin:
// Connect the Data line here. A 4.7kΩ pull-up resistor between Data and 3.3V is required.
#define ONE_WIRE_BUS            4

// 5V Single-Channel Relay Module Control Pin:
#define RELAY_PIN               26

// ==============================================================================
// 2. WI-FI & FLASK SERVER CONFIGURATION
// ==============================================================================
// Set your Wi-Fi credentials (or the Access Point credentials of the server ESP)
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Target Flask Server Endpoint:
// Replace the IP address and port with your Flask server's IP and port.
// Example for standard LAN:  "http://192.168.1.150:5000/api/sensor-data"
// Example for ESP SoftAP:    "http://192.168.4.1:5000/api/sensor-data"
const char* FLASK_SERVER_URL = "http://192.168.1.100:5000/api/sensor-data";

// HTTP Request Timeout in milliseconds
#define HTTP_TIMEOUT_MS         2000

// ==============================================================================
// 3. HARDWARE CONFIGURATION & THRESHOLDS
// ==============================================================================
// Relay Logic:
// Most 5V single-channel relay modules are ACTIVE-LOW (LOW = Relay ON, HIGH = Relay OFF).
// If your module is ACTIVE-HIGH, change this to false.
#define RELAY_ACTIVE_LOW        true

// Temperature Alert Thresholds (Celsius):
// Triggers the safety relay if temperature exceeds this limit.
#define TEMP_ALERT_THRESHOLD    50.0f  // Trip temperature (°C)
#define TEMP_HYSTERESIS          3.0f  // Temperature must drop below (50 - 3 = 47°C) to clear

// MQ-135 Gas Sensor Thresholds for ESP32 (12-bit ADC: 0 - 4095):
// Equivalent 10-bit values in parentheses for reference:
#define GAS_THRESHOLD_SAFE      1200   // < 1200 (~300 on 10-bit): Normal / Clean air
#define GAS_THRESHOLD_MODERATE  2000   // 1200 - 2000 (~300-500): Moderate / Low Risk
#define GAS_THRESHOLD_HEAVY     2800   // 2000 - 2800 (~500-700): Heavy Risk (Triggers Relay)
                                       // > 2800 (>700): Extreme Danger (Triggers Relay)
#define GAS_HYSTERESIS           200   // Hysteresis buffer to prevent relay chattering

// Timing & Non-blocking intervals (milliseconds)
#define SENSOR_READ_INTERVAL_MS 2000   // Read and report every 2 seconds
#define MQ135_WARMUP_MS        30000   // 30-second initial stabilization reminder

// ==============================================================================
// 4. GLOBAL OBJECTS & STATE
// ==============================================================================
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

// Fire and gas risk levels
enum RiskLevel {
  RISK_SAFE = 0,       // Normal
  RISK_LOW = 1,        // Moderate / Low risk
  RISK_HEAVY = 2,      // Heavy risk
  RISK_EXTREME = 3     // Extreme danger
};

int fire_warning = RISK_SAFE;
String thermal_message = "";
unsigned long lastReadTime = 0;
bool relayState = false;

// ==============================================================================
// 5. RELAY CONTROL FUNCTIONS
// ==============================================================================
/**
 * @brief Sets the state of the 5V Relay module, respecting Active-LOW / Active-HIGH logic.
 * @param turnOn true to energize/activate the relay, false to de-energize.
 */
void setRelay(bool turnOn) {
  relayState = turnOn;
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PIN, turnOn ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PIN, turnOn ? HIGH : LOW);
  }
}

// ==============================================================================
// 6. WI-FI & FLASK TELEMETRY TRANSMISSION
// ==============================================================================
/**
 * @brief Sends sensor readings and relay state to the Flask server via HTTP POST JSON.
 */
void sendTelemetryToFlask(int gasRaw, int gasScaled, float gasVoltage, int riskLvl,
                          const String& riskStr, float tempC, float tempF,
                          bool tempValid, bool rState, const String& rReason) {
  // Check Wi-Fi connection
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[WIFI] Not connected. Telemetry not sent (will retry next cycle)."));
    return;
  }

  HTTPClient http;
  http.begin(FLASK_SERVER_URL);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  // Construct JSON payload
  String jsonPayload = "{";
  jsonPayload += "\"gas_raw\":" + String(gasRaw) + ",";
  jsonPayload += "\"gas_scaled\":" + String(gasScaled) + ",";
  jsonPayload += "\"gas_voltage\":" + String(gasVoltage, 2) + ",";
  jsonPayload += "\"risk_level\":" + String(riskLvl) + ",";
  jsonPayload += "\"risk_status\":\"" + riskStr + "\",";
  if (tempValid) {
    jsonPayload += "\"temperature_c\":" + String(tempC, 2) + ",";
    jsonPayload += "\"temperature_f\":" + String(tempF, 2) + ",";
  } else {
    jsonPayload += "\"temperature_c\":null,";
    jsonPayload += "\"temperature_f\":null,";
  }
  jsonPayload += "\"relay_active\":" + String(rState ? "true" : "false") + ",";
  jsonPayload += "\"relay_reason\":\"" + rReason + "\",";
  jsonPayload += "\"uptime_ms\":" + String(millis());
  jsonPayload += "}";

  // Send HTTP POST
  int httpResponseCode = http.POST(jsonPayload);

  if (httpResponseCode > 0) {
    Serial.print(F("[FLASK SERVER] HTTP POST response code: "));
    Serial.println(httpResponseCode);
    if (httpResponseCode == HTTP_CODE_OK || httpResponseCode == 201) {
      String response = http.getString();
      if (response.length() > 0) {
        Serial.print(F("[FLASK SERVER] Response payload: "));
        Serial.println(response);
      }
    }
  } else {
    Serial.print(F("[FLASK SERVER] HTTP POST failed, error: "));
    Serial.println(http.errorToString(httpResponseCode).c_str());
  }

  http.end();
}

// ==============================================================================
// 7. SETUP
// ==============================================================================
void setup() {
  // Initialize Serial Monitor (115200 baud)
  Serial.begin(115200);
  delay(1000); // Allow serial line to stabilize

  Serial.println();
  Serial.println(F("=========================================================="));
  Serial.println(F("   Warehouse Hazardous Gas & Temperature Monitor (ESP32)  "));
  Serial.println(F("   Communicating with external Flask Server               "));
  Serial.println(F("=========================================================="));

  // Initialize GPIOs
  pinMode(MQ135_ANALOG_PIN, INPUT);
  
  // Set relay pin to OUTPUT and immediately force it to the safe OFF state
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);

  // Set ADC resolution to 12 bits (ESP32 standard: 0 to 4095)
  analogReadResolution(12);

  // Initialize DS18B20 1-Wire Temperature Sensor
  Serial.print(F("[INFO] Initializing DS18B20 Temperature Sensor on GPIO "));
  Serial.print(ONE_WIRE_BUS);
  Serial.print(F("... "));
  tempSensor.begin();
  
  int deviceCount = tempSensor.getDeviceCount();
  if (deviceCount == 0) {
    Serial.println(F("\n[WARNING] No DS18B20 sensor detected!"));
    Serial.println(F("          Please verify wiring and confirm the 4.7kΩ pull-up resistor"));
    Serial.println(F("          is connected between DATA (GPIO 4) and 3.3V."));
  } else {
    Serial.print(F("[OK] Found "));
    Serial.print(deviceCount);
    Serial.println(F(" sensor(s)."));
  }

  Serial.print(F("[INFO] 5V Relay module initialized on GPIO "));
  Serial.print(RELAY_PIN);
  Serial.println(F(" (Initial State: OFF)."));

  // Connect to Wi-Fi
  Serial.print(F("[WIFI] Connecting to SSID: "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println(F("[INFO] MQ-135 Gas Sensor warming up (heater coil active)."));
  Serial.println(F("[INFO] Monitoring initialized successfully."));
  Serial.println(F("==========================================================\n"));
}

// ==============================================================================
// 8. MAIN LOOP
// ==============================================================================
void loop() {
  unsigned long currentMillis = millis();

  // Execute periodic reading without blocking loop()
  if (currentMillis - lastReadTime >= SENSOR_READ_INTERVAL_MS || lastReadTime == 0) {
    lastReadTime = currentMillis;

    // ------------------------------------------------------------------------
    // A. READ MQ-135 GAS SENSOR
    // ------------------------------------------------------------------------
    // Read raw 12-bit ADC value (0 - 4095)
    int gasLevel = analogRead(MQ135_ANALOG_PIN);

    // Approximate analog voltage at ADC pin (ESP32 3.3V reference)
    float gasVoltage = (gasLevel / 4095.0f) * 3.3f;

    // Equivalent 10-bit scaled value (0 - 1023) for reference
    int gasLevel10Bit = map(gasLevel, 0, 4095, 0, 1023);

    // Classify Gas / Fire Risk Level
    String gasRiskStr;
    if (gasLevel < GAS_THRESHOLD_SAFE) {
      fire_warning = RISK_SAFE;
      gasRiskStr = "SAFE (Normal Air Quality)";
    } else if (gasLevel < GAS_THRESHOLD_MODERATE) {
      fire_warning = RISK_LOW;
      gasRiskStr = "MODERATE (Low Risk)";
    } else if (gasLevel < GAS_THRESHOLD_HEAVY) {
      fire_warning = RISK_HEAVY;
      gasRiskStr = "HEAVY RISK (Hazardous Gas Detected!)";
    } else {
      fire_warning = RISK_EXTREME;
      gasRiskStr = "EXTREME RISK (DANGER - Evacuate/Ventilate!)";
    }

    // ------------------------------------------------------------------------
    // B. READ DS18B20 TEMPERATURE SENSOR
    // ------------------------------------------------------------------------
    tempSensor.requestTemperatures();
    float tempC = tempSensor.getTempCByIndex(0);
    float tempF = 0.0f;
    bool tempValid = true;

    // Check for sensor fault or disconnection (DEVICE_DISCONNECTED_C is -127.0°C)
    if (tempC == DEVICE_DISCONNECTED_C || isnan(tempC) || tempC < -50.0f) {
      tempValid = false;
      thermal_message = "FAULT: Sensor Disconnected / Wiring Error";
    } else {
      tempF = DallasTemperature::toFahrenheit(tempC);
      if (tempC >= TEMP_ALERT_THRESHOLD) {
        thermal_message = "CRITICAL: High Temperature Alert!";
      } else {
        thermal_message = "Normal";
      }
    }

    // ------------------------------------------------------------------------
    // C. RELAY AUTOMATION LOGIC (WITH HYSTERESIS)
    // ------------------------------------------------------------------------
    // Local safety is evaluated BEFORE sending network telemetry!
    // Trip condition:
    //   - Gas Level is HEAVY or EXTREME (>= GAS_THRESHOLD_HEAVY) OR
    //   - Temperature exceeds safety threshold (>= TEMP_ALERT_THRESHOLD)
    // Release condition (with hysteresis to prevent relay clicking/flutter):
    //   - Gas Level drops below (GAS_THRESHOLD_HEAVY - GAS_HYSTERESIS) AND
    //   - Temperature drops below (TEMP_ALERT_THRESHOLD - TEMP_HYSTERESIS)

    bool gasAlertActive = false;
    bool tempAlertActive = false;
    String relayReason = "";

    // Gas alert check with hysteresis
    if (!relayState && gasLevel >= GAS_THRESHOLD_HEAVY) {
      gasAlertActive = true;
    } else if (relayState && gasLevel >= (GAS_THRESHOLD_HEAVY - GAS_HYSTERESIS)) {
      gasAlertActive = true;
    }

    // Temperature alert check with hysteresis
    if (tempValid) {
      if (!relayState && tempC >= TEMP_ALERT_THRESHOLD) {
        tempAlertActive = true;
      } else if (relayState && tempC >= (TEMP_ALERT_THRESHOLD - TEMP_HYSTERESIS)) {
        tempAlertActive = true;
      }
    }

    // Determine final relay action
    bool shouldTriggerRelay = gasAlertActive || tempAlertActive;

    if (gasAlertActive) {
      relayReason += (fire_warning == RISK_EXTREME) ? "Extreme Gas Level" : "Heavy Gas Level";
    }
    if (tempAlertActive) {
      if (relayReason.length() > 0) relayReason += " + ";
      relayReason += "Over-Temperature";
    }
    if (!shouldTriggerRelay) {
      relayReason = "Normal operating conditions";
    }

    setRelay(shouldTriggerRelay);

    // ------------------------------------------------------------------------
    // D. SERIAL MONITOR LOGGING
    // ------------------------------------------------------------------------
    Serial.println(F("---------------- [ Warehouse Status ] ----------------"));

    // Warmup notice during first 30 seconds
    if (currentMillis < MQ135_WARMUP_MS) {
      Serial.print(F("[WARMUP] MQ-135 heating... ("));
      Serial.print((MQ135_WARMUP_MS - currentMillis) / 1000);
      Serial.println(F("s remaining)"));
    }

    // Gas Sensor readings
    Serial.print(F("MQ-135 Gas Level    : Raw: "));
    Serial.print(gasLevel);
    Serial.print(F(" (12-bit) | Scaled: "));
    Serial.print(gasLevel10Bit);
    Serial.print(F(" (10-bit) | "));
    Serial.print(gasVoltage, 2);
    Serial.println(F(" V"));

    Serial.print(F("Gas Risk Level      : ["));
    Serial.print(fire_warning);
    Serial.print(F("] "));
    Serial.println(gasRiskStr);

    // Temperature readings
    Serial.print(F("DS18B20 Temperature : "));
    if (tempValid) {
      Serial.print(tempC, 2);
      Serial.print(F(" °C  ("));
      Serial.print(tempF, 2);
      Serial.print(F(" °F) - Status: "));
      Serial.println(thermal_message);
    } else {
      Serial.println(thermal_message);
    }

    // Relay status
    Serial.print(F("5V Relay Actuator   : "));
    if (relayState) {
      Serial.print(F(">>> [ACTIVATED / ON] <<< Reason: "));
      Serial.println(relayReason);
    } else {
      Serial.println(F("[OFF / SAFE] Normal operating conditions"));
    }

    // Wi-Fi Connection status
    Serial.print(F("Wi-Fi Status        : "));
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print(F("Connected (IP: "));
      Serial.print(WiFi.localIP());
      Serial.println(F(")"));
    } else {
      Serial.println(F("Connecting / Disconnected"));
    }

    // ------------------------------------------------------------------------
    // E. SEND TELEMETRY TO FLASK SERVER
    // ------------------------------------------------------------------------
    sendTelemetryToFlask(gasLevel, gasLevel10Bit, gasVoltage, fire_warning,
                         gasRiskStr, tempC, tempF, tempValid, relayState, relayReason);

    Serial.println(F("------------------------------------------------------\n"));
  }
}