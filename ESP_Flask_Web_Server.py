"""
ESP32 Flask Web Server
Receives live telemetry from the Sensor ESP32 (MQ-135 + DS18B20 + Relay)
and provides a real-time web dashboard and REST API.

Run with:
    python3 ESP_Flask_Web_Server.py
Or on an external server / Raspberry Pi:
    flask run --host=0.0.0.0 --port=5000
"""

from flask import Flask, request, jsonify, render_template_string
from datetime import datetime

app = Flask(__name__)

# In-memory storage for latest telemetry and history log
latest_telemetry = {
    "gas_raw": None,
    "gas_scaled": None,
    "gas_voltage": None,
    "risk_level": 0,
    "risk_status": "Waiting for sensor data...",
    "temperature_c": None,
    "temperature_f": None,
    "relay_active": False,
    "relay_reason": "N/A",
    "uptime_ms": 0,
    "last_updated": None
}

telemetry_history = []
MAX_HISTORY_LEN = 50

DASHBOARD_HTML = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Warehouse Environmental Monitor</title>
    <meta http-equiv="refresh" content="3">
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
        body { background: #0f172a; color: #f8fafc; padding: 24px; }
        .container { max-width: 900px; margin: 0 auto; }
        header { text-align: center; margin-bottom: 28px; }
        h1 { font-size: 26px; color: #38bdf8; margin-bottom: 6px; }
        .subtitle { color: #94a3b8; font-size: 14px; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 16px; margin-bottom: 24px; }
        .card { background: #1e293b; border-radius: 12px; padding: 20px; border: 1px solid #334155; }
        .card h2 { font-size: 13px; text-transform: uppercase; letter-spacing: 0.05em; color: #94a3b8; margin-bottom: 12px; }
        .value { font-size: 32px; font-weight: bold; margin-bottom: 8px; }
        .subtext { font-size: 13px; color: #cbd5e1; }
        .badge { display: inline-block; padding: 4px 10px; border-radius: 9999px; font-size: 12px; font-weight: 600; }
        .badge-safe { background: #065f46; color: #6ee7b7; }
        .badge-low { background: #854d0e; color: #fde047; }
        .badge-heavy { background: #9a3412; color: #fdba74; }
        .badge-extreme { background: #991b1b; color: #fca5a5; }
        .badge-relay-on { background: #dc2626; color: white; animation: pulse 1.5s infinite; }
        .badge-relay-off { background: #1e293b; color: #94a3b8; border: 1px solid #475569; }
        @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.6; } }
        table { width: 100%; border-collapse: collapse; background: #1e293b; border-radius: 12px; overflow: hidden; border: 1px solid #334155; }
        th, td { padding: 10px 14px; text-align: left; font-size: 13px; border-bottom: 1px solid #334155; }
        th { background: #0f172a; color: #94a3b8; font-weight: 600; }
        tr:last-child td { border-bottom: none; }
        .footer { text-align: center; margin-top: 20px; font-size: 12px; color: #64748b; }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>🏭 Warehouse Environmental Monitor</h1>
            <p class="subtitle">Live Telemetry from ESP32 Sensor Node (Auto-refreshes every 3s)</p>
            <p class="subtitle" style="margin-top: 4px;">Last Received: <strong>{{ data.last_updated or 'Never' }}</strong></p>
        </header>

        <div class="grid">
            <!-- Gas Sensor Card -->
            <div class="card">
                <h2>MQ-135 Gas Sensor</h2>
                <div class="value">
                    {{ data.gas_raw if data.gas_raw is not none else '--' }}
                    <span style="font-size: 16px; color: #64748b;">(12-bit)</span>
                </div>
                <div class="subtext" style="margin-bottom: 10px;">
                    Scaled 10-bit: <strong>{{ data.gas_scaled if data.gas_scaled is not none else '--' }}</strong> |
                    Voltage: <strong>{{ "%.2f"|format(data.gas_voltage|float) if data.gas_voltage is not none else '--' }} V</strong>
                </div>
                <div>
                    {% if data.risk_level == 0 %}
                        <span class="badge badge-safe">SAFE (Normal)</span>
                    {% elif data.risk_level == 1 %}
                        <span class="badge badge-low">MODERATE RISK</span>
                    {% elif data.risk_level == 2 %}
                        <span class="badge badge-heavy">HEAVY RISK</span>
                    {% else %}
                        <span class="badge badge-extreme">EXTREME DANGER</span>
                    {% endif %}
                </div>
            </div>

            <!-- Temperature Card -->
            <div class="card">
                <h2>DS18B20 Temperature</h2>
                <div class="value">
                    {% if data.temperature_c is not none %}
                        {{ "%.1f"|format(data.temperature_c|float) }} <span style="font-size: 18px;">°C</span>
                    {% else %}
                        -- °C
                    {% endif %}
                </div>
                <div class="subtext">
                    {% if data.temperature_f is not none %}
                        Fahrenheit: <strong>{{ "%.1f"|format(data.temperature_f|float) }} °F</strong>
                    {% else %}
                        Fahrenheit: <strong>-- °F</strong>
                    {% endif %}
                </div>
                <div class="subtext" style="margin-top: 10px;">
                    {% if data.temperature_c is not none and data.temperature_c|float >= 50.0 %}
                        <span class="badge badge-extreme">HIGH TEMPERATURE ALERT</span>
                    {% elif data.temperature_c is not none %}
                        <span class="badge badge-safe">Normal Temperature</span>
                    {% else %}
                        <span class="badge badge-safe">Waiting for Temperature</span>
                    {% endif %}
                </div>
            </div>

            <!-- Relay Module Card -->
            <div class="card">
                <h2>5V Relay Actuator</h2>
                <div class="value">
                    {% if data.relay_active %}
                        <span style="color: #f87171;">ACTIVE / ON</span>
                    {% else %}
                        <span style="color: #4ade80;">OFF</span>
                    {% endif %}
                </div>
                <div class="subtext" style="margin-bottom: 10px;">
                    Reason: <strong>{{ data.relay_reason }}</strong>
                </div>
                <div>
                    {% if data.relay_active %}
                        <span class="badge badge-relay-on">RELAY ENERGIZED</span>
                    {% else %}
                        <span class="badge badge-relay-off">RELAY IDLE (SAFE)</span>
                    {% endif %}
                </div>
            </div>
        </div>

        <!-- Recent Logs Table -->
        <h2 style="font-size: 16px; margin-bottom: 10px; color: #cbd5e1;">Recent Telemetry History</h2>
        <table>
            <thead>
                <tr>
                    <th>Timestamp</th>
                    <th>Gas (Raw / 10-bit)</th>
                    <th>Risk Level</th>
                    <th>Temperature</th>
                    <th>Relay State</th>
                </tr>
            </thead>
            <tbody>
                {% for entry in history %}
                <tr>
                    <td>{{ entry.last_updated }}</td>
                    <td>{{ entry.gas_raw if entry.gas_raw is not none else '--' }} ({{ entry.gas_scaled if entry.gas_scaled is not none else '--' }})</td>
                    <td>{{ entry.risk_status }}</td>
                    <td>{{ "%.1f"|format(entry.temperature_c|float) if entry.temperature_c is not none else 'N/A' }} °C</td>
                    <td>{{ 'ON' if entry.relay_active else 'OFF' }}</td>
                </tr>
                {% else %}
                <tr>
                    <td colspan="5" style="text-align: center; color: #64748b;">No data received yet. Connect the Sensor ESP32.</td>
                </tr>
                {% endfor %}
            </tbody>
        </table>

        <div class="footer">
            Endpoint for ESP32 HTTP POST: <code>POST /api/sensor-data</code>
        </div>
    </div>
</body>
</html>
"""

@app.route("/", methods=["GET"])
def dashboard():
    return render_template_string(DASHBOARD_HTML, data=latest_telemetry, history=telemetry_history)

@app.route("/api/sensor-data", methods=["POST"])
def receive_sensor_data():
    global latest_telemetry, telemetry_history
    data = request.get_json(force=True, silent=True)
    if not isinstance(data, dict):
        return jsonify({"status": "error", "message": "Invalid JSON or expected JSON object"}), 400

    data["last_updated"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    latest_telemetry.update(data)

    telemetry_history.insert(0, dict(latest_telemetry))
    if len(telemetry_history) > MAX_HISTORY_LEN:
        telemetry_history.pop()

    return jsonify({
        "status": "success",
        "received_at": latest_telemetry["last_updated"],
        "relay_acknowledged": latest_telemetry.get("relay_active", False)
    }), 200

@app.route("/api/latest", methods=["GET"])
def get_latest():
    return jsonify(latest_telemetry), 200

if __name__ == "__main__":
    print("==================================================")
    print("  ESP32 Warehouse Monitor - Flask Telemetry Server")
    print("  Listening on http://0.0.0.0:5000")
    print("  Endpoint: POST http://<IP>:5000/api/sensor-data")
    print("==================================================")
    app.run(host="0.0.0.0", port=5000, debug=False)
