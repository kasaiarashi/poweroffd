#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod connection;
mod ping;
mod wol;

use app::App;

fn main() -> eframe::Result<()> {
    let native_options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([720.0, 560.0])
            .with_min_inner_size([520.0, 400.0])
            .with_title("poweroffd"),
        ..Default::default()
    };
    eframe::run_native(
        "poweroffd-gui",
        native_options,
        Box::new(|cc| Ok(Box::new(App::new(cc)))),
    )
}
