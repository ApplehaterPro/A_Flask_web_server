use eframe::egui;

fn main() -> eframe::Result<()> {
    let options = eframe::NativeOptions::default();

    eframe::run_native(
        "My Calculator",
        options,
        Box::new(|_cc| Box::new(MyApp::default())),
    )
}

struct MyApp {
    /// Holds the current string typed or entered via buttons (e.g., "3 + 5")
    input: String,
    /// Holds the evaluation result or error message to display
    result: String,
}

 Deimplfault for MyApp {
    fn default() -> Self {
        Self {
            input: String::new(),
            result: String::from("0"),
        }
    }
}

impl MyApp {
    /// Evaluates a string expression like "3 + 5"
    fn evaluate_expression(&mut self) {
        let parts: Vec<&str> = self.input.trim().split_whitespace().collect();

        if parts.len() != 3 {
            self.result = String::from("Error: Use format 'num op num'");
            return;
        }

        let num1: f64 = match parts[0].parse() {
            Ok(n) => n,
            Err(_) => {
                self.result = String::from("Error: Invalid 1st num");
                return;
            }
        };

        let op = parts[1];

        let num2: f64 = match parts[2].parse() {
            Ok(n) => n,
            Err(_) => {
                self.result = String::from("Error: Invalid 2nd num");
                return;
            }
        };

        let calculation = match op {
            "+" => num1 + num2,
            "-" => num1 - num2,
            "*" | "x" => num1 * num2,
            "/" => {
                if num2 == 0.0 {
                    self.result = String::from("Error: Div by 0");
                    return;
                }
                num1 / num2
            }
            _ => {
                self.result = format!("Error: Unknown op '{}'", op);
                return;
            }
        };

        self.result = calculation.to_string();
    }
}

impl eframe::App for MyApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        egui::CentralPanel::default().show(ctx, |ui| {
            ui.heading("GUI Calculator");
            ui.add_space(10.0);

            // Text input field so users can either type or use buttons
            ui.horizontal(|ui| {
                ui.label("Expression:");
                ui.text_edit_singleline(&mut self.input);
            });

            ui.add_space(10.0);

            // Grid for standard calculator buttons
            egui::Grid::new("calculator_grid")
                .spacing([5.0, 5.0])
                .show(ui, |ui| {
                    // Row 1
                    if ui.button("7").clicked() { self.input.push('7'); }
                    if ui.button("8").clicked() { self.input.push('8'); }
                    if ui.button("9").clicked() { self.input.push('9'); }
                    if ui.button(" / ").clicked() { self.input.push_str(" / "); }
                    ui.end_row();

                    // Row 2
                    if ui.button("4").clicked() { self.input.push('4'); }
                    if ui.button("5").clicked() { self.input.push('5'); }
                    if ui.button("6").clicked() { self.input.push('6'); }
                    if ui.button(" * ").clicked() { self.input.push_str(" * "); }
                    ui.end_row();

                    // Row 3
                    if ui.button("1").clicked() { self.input.push('1'); }
                    if ui.button("2").clicked() { self.input.push('2'); }
                    if ui.button("3").clicked() { self.input.push('3'); }
                    if ui.button(" - ").clicked() { self.input.push_str(" - "); }
                    ui.end_row();

                    // Row 4
                    if ui.button("C").clicked() { 
                        self.input.clear(); 
                        self.result = String::from("0");
                    }
                    if ui.button("0").clicked() { self.input.push('0'); }
                    if ui.button("=").clicked() { self.evaluate_expression(); }
                    if ui.button(" + ").clicked() { self.input.push_str(" + "); }
                    ui.end_row();
                });

            ui.add_space(15.0);
            
            // Output Display
            ui.horizontal(|ui| {
                ui.label("Result:");
                ui.colored_label(egui::Color32::LIGHT_GREEN, &self.result);
            });
        });
    }
}
