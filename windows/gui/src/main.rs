#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod connection;
mod ping;
mod wol;

use app::App;

/// Simple generated app icon: green rounded square with a white power ring.
fn app_icon() -> egui::IconData {
    const N: u32 = 64;
    let mut rgba = vec![0u8; (N * N * 4) as usize];
    let c = (N as f32 - 1.0) / 2.0;
    for y in 0..N {
        for x in 0..N {
            let (dx, dy) = (x as f32 - c, y as f32 - c);
            let (ax, ay) = (dx.abs(), dy.abs());
            let in_square = ax <= 29.0 && ay <= 29.0 && !(ax > 21.0 && ay > 21.0 && (ax - 21.0).hypot(ay - 21.0) > 8.0);
            if !in_square {
                continue;
            }
            let r = dx.hypot(dy);
            let ring = (15.0..=20.0).contains(&r) && !(dy < 0.0 && ax < 4.0);
            let bar = ax <= 2.5 && (-21.0..=-5.0).contains(&dy);
            let px = if ring || bar { [255, 255, 255, 255] } else { [38, 140, 76, 255] };
            let i = ((y * N + x) * 4) as usize;
            rgba[i..i + 4].copy_from_slice(&px);
        }
    }
    egui::IconData { rgba, width: N, height: N }
}

fn main() -> eframe::Result<()> {
    let native_options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([720.0, 560.0])
            .with_min_inner_size([520.0, 400.0])
            .with_title("poweroffd")
            .with_icon(std::sync::Arc::new(app_icon())),
        ..Default::default()
    };
    eframe::run_native(
        "poweroffd-gui",
        native_options,
        Box::new(|cc| Ok(Box::new(App::new(cc)))),
    )
}
