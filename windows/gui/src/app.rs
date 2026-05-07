use std::collections::HashMap;
use std::sync::mpsc::{channel, Receiver, Sender};
use std::thread;
use std::time::{Duration, Instant};

use eframe::egui;

use crate::connection::{self, Connection};
use crate::{ping, wol};

#[derive(Clone, Debug)]
pub enum PingResult {
    Pending,
    Up(u32),
    Down(String),
}

pub enum AppEvent {
    Ping(usize, PingResult),
    Status(String),
}

#[derive(Clone, Copy)]
pub enum ActionKind {
    Wake,
    Shutdown,
    Lock,
}

pub struct App {
    connections: Vec<Connection>,
    ping_status: HashMap<usize, PingResult>,
    selected: Option<usize>,
    log: Vec<String>,
    dirty: bool,
    last_auto_ping: Option<Instant>,
    confirm_delete: Option<usize>,
    tx: Sender<AppEvent>,
    rx: Receiver<AppEvent>,
}

const AUTO_PING_INTERVAL: Duration = Duration::from_secs(30);

impl App {
    pub fn new(_cc: &eframe::CreationContext<'_>) -> Self {
        let connections = connection::load();
        let (tx, rx) = channel();
        Self {
            connections,
            ping_status: HashMap::new(),
            selected: None,
            log: Vec::new(),
            dirty: false,
            last_auto_ping: None,
            confirm_delete: None,
            tx,
            rx,
        }
    }

    fn maybe_auto_ping(&mut self) {
        if self.connections.is_empty() {
            return;
        }
        let now = Instant::now();
        let due = match self.last_auto_ping {
            None => true,
            Some(t) => now.duration_since(t) >= AUTO_PING_INTERVAL,
        };
        if due {
            for i in 0..self.connections.len() {
                self.spawn_ping(i);
            }
            self.last_auto_ping = Some(now);
        }
    }

    fn save_to_disk(&self) {
        if let Err(e) = connection::save(&self.connections) {
            eprintln!("save error: {}", e);
        }
    }

    fn push_log(&mut self, line: String) {
        if self.log.len() > 200 {
            self.log.drain(0..50);
        }
        let ts = chrono_like_now();
        self.log.push(format!("{}  {}", ts, line));
    }

    fn drain_events(&mut self) {
        while let Ok(ev) = self.rx.try_recv() {
            match ev {
                AppEvent::Ping(i, r) => {
                    self.ping_status.insert(i, r);
                }
                AppEvent::Status(s) => self.push_log(s),
            }
        }
    }

    fn spawn_ping(&mut self, idx: usize) {
        let Some(conn) = self.connections.get(idx) else {
            return;
        };
        let host = conn.host.clone();
        let tx = self.tx.clone();
        self.ping_status.insert(idx, PingResult::Pending);
        thread::spawn(move || {
            let result = match ping::ping(&host, 1500) {
                Ok(rtt) => PingResult::Up(rtt),
                Err(e) => PingResult::Down(e),
            };
            let _ = tx.send(AppEvent::Ping(idx, result));
        });
    }

    fn spawn_action(&self, conn: Connection, kind: ActionKind) {
        let tx = self.tx.clone();
        thread::spawn(move || {
            let outcome = run_action(&conn, kind);
            let _ = tx.send(AppEvent::Status(outcome));
        });
    }
}

fn run_action(c: &Connection, k: ActionKind) -> String {
    match k {
        ActionKind::Wake => {
            if c.wake_mac.trim().is_empty() {
                return format!("[{}] wake failed: wake MAC not set", c.name);
            }
            let mac = match wol::parse_mac(&c.wake_mac) {
                Ok(m) => m,
                Err(e) => return format!("[{}] wake: {}", c.name, e),
            };
            let pkt = wol::build_packet(mac, "");
            let target = if c.broadcast.trim().is_empty() {
                "255.255.255.255"
            } else {
                c.broadcast.as_str()
            };
            match wol::send(target, c.port, &pkt, true) {
                Ok(addr) => format!("[{}] wake → {} ({} bytes)", c.name, addr, pkt.len()),
                Err(e) => format!("[{}] wake failed: {}", c.name, e),
            }
        }
        ActionKind::Shutdown => {
            if c.shutdown_mac.trim().is_empty() {
                return format!("[{}] shutdown failed: shutdown MAC not set", c.name);
            }
            let mac = match wol::parse_mac(&c.shutdown_mac) {
                Ok(m) => m,
                Err(e) => return format!("[{}] shutdown: {}", c.name, e),
            };
            let pkt = wol::build_packet(mac, &c.secret);
            match wol::send(&c.host, c.port, &pkt, false) {
                Ok(addr) => format!("[{}] shutdown → {} ({} bytes)", c.name, addr, pkt.len()),
                Err(e) => format!("[{}] shutdown failed: {}", c.name, e),
            }
        }
        ActionKind::Lock => {
            if c.lock_mac.trim().is_empty() {
                return format!("[{}] lock failed: lock MAC not set", c.name);
            }
            let mac = match wol::parse_mac(&c.lock_mac) {
                Ok(m) => m,
                Err(e) => return format!("[{}] lock: {}", c.name, e),
            };
            let pkt = wol::build_packet(mac, &c.secret);
            match wol::send(&c.host, c.port, &pkt, false) {
                Ok(addr) => format!("[{}] lock → {} ({} bytes)", c.name, addr, pkt.len()),
                Err(e) => format!("[{}] lock failed: {}", c.name, e),
            }
        }
    }
}

fn chrono_like_now() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    let secs = now.as_secs();
    let h = (secs / 3600) % 24;
    let m = (secs / 60) % 60;
    let s = secs % 60;
    format!("{:02}:{:02}:{:02}", h, m, s)
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.drain_events();
        self.maybe_auto_ping();
        ctx.request_repaint_after(Duration::from_millis(500));

        // Top bar
        egui::TopBottomPanel::top("top").show(ctx, |ui| {
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                ui.heading("poweroffd");
                if self.dirty {
                    ui.colored_label(
                        egui::Color32::from_rgb(230, 180, 80),
                        "● unsaved",
                    );
                }
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if ui.button("+ New").clicked() {
                        self.connections.push(Connection::template());
                        self.selected = Some(self.connections.len() - 1);
                        self.dirty = true;
                    }
                    if ui.button("Refresh all").clicked() {
                        let started = Instant::now();
                        for i in 0..self.connections.len() {
                            self.spawn_ping(i);
                        }
                        self.last_auto_ping = Some(Instant::now());
                        self.push_log(format!(
                            "ping {} hosts ({}ms to dispatch)",
                            self.connections.len(),
                            started.elapsed().as_millis()
                        ));
                    }
                });
            });
            ui.add_space(4.0);
        });

        // Bottom log
        egui::TopBottomPanel::bottom("log")
            .resizable(true)
            .default_height(140.0)
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    ui.label("Log");
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if ui.small_button("Clear").clicked() {
                            self.log.clear();
                        }
                    });
                });
                ui.separator();
                egui::ScrollArea::vertical()
                    .stick_to_bottom(true)
                    .auto_shrink([false; 2])
                    .show(ui, |ui| {
                        for line in &self.log {
                            ui.monospace(line);
                        }
                    });
            });

        // Left list
        egui::SidePanel::left("list")
            .default_width(220.0)
            .resizable(true)
            .show(ctx, |ui| {
                ui.label(egui::RichText::new("Connections").strong());
                ui.separator();

                let count = self.connections.len();
                let names: Vec<String> = self.connections.iter().map(|c| c.name.clone()).collect();
                let selected = self.selected;
                let mut request_delete: Option<usize> = None;
                let mut newly_selected: Option<usize> = None;

                egui::ScrollArea::vertical().auto_shrink([false; 2]).show(ui, |ui| {
                    for i in 0..count {
                        let status = self
                            .ping_status
                            .get(&i)
                            .cloned()
                            .unwrap_or(PingResult::Pending);
                        let (dot, color) = match status {
                            PingResult::Up(_) => ("●", egui::Color32::from_rgb(80, 200, 120)),
                            PingResult::Down(_) => ("●", egui::Color32::from_rgb(200, 80, 80)),
                            PingResult::Pending => ("·", egui::Color32::GRAY),
                        };
                        ui.horizontal(|ui| {
                            ui.colored_label(color, dot);
                            let name = if names[i].is_empty() {
                                "(unnamed)".to_string()
                            } else {
                                names[i].clone()
                            };
                            let resp = ui.selectable_label(selected == Some(i), name);
                            if resp.clicked() {
                                newly_selected = Some(i);
                            }
                            resp.context_menu(|ui| {
                                if ui.button("Delete…").clicked() {
                                    request_delete = Some(i);
                                    ui.close_menu();
                                }
                            });
                        });
                    }
                });

                if let Some(i) = newly_selected {
                    self.selected = Some(i);
                }
                if let Some(i) = request_delete {
                    self.confirm_delete = Some(i);
                }
            });

        // Center: form + actions
        egui::CentralPanel::default().show(ctx, |ui| {
            let Some(idx) = self.selected else {
                ui.centered_and_justified(|ui| {
                    ui.label("Select a connection on the left, or click + New");
                });
                return;
            };
            if idx >= self.connections.len() {
                self.selected = None;
                return;
            }

            let mut changed = false;

            ui.label(egui::RichText::new("Details").strong());
            ui.separator();

            egui::Grid::new("form")
                .num_columns(2)
                .spacing([10.0, 6.0])
                .show(ui, |ui| {
                    let c = &mut self.connections[idx];

                    ui.label("Name");
                    changed |= ui.text_edit_singleline(&mut c.name).changed();
                    ui.end_row();

                    ui.label("Host (IP/hostname)");
                    changed |= ui
                        .add(egui::TextEdit::singleline(&mut c.host).hint_text("192.168.1.10"))
                        .changed();
                    ui.end_row();

                    ui.label("Broadcast (Wake)");
                    changed |= ui
                        .add(
                            egui::TextEdit::singleline(&mut c.broadcast)
                                .hint_text("255.255.255.255"),
                        )
                        .changed();
                    ui.end_row();

                    ui.label("Wake MAC")
                        .on_hover_text("MAC the NIC firmware matches to wake on a magic packet (usually the real NIC MAC)");
                    changed |= ui
                        .add(egui::TextEdit::singleline(&mut c.wake_mac).hint_text("AA:BB:CC:DD:EE:FF"))
                        .changed();
                    ui.end_row();

                    ui.label("Shutdown MAC")
                        .on_hover_text("MAC poweroffd matches to trigger shutdown");
                    changed |= ui
                        .add(egui::TextEdit::singleline(&mut c.shutdown_mac).hint_text("AA:BB:CC:DD:EE:FF"))
                        .changed();
                    ui.end_row();

                    ui.label("Lock MAC")
                        .on_hover_text("MAC poweroffd matches to trigger screen lock");
                    changed |= ui
                        .add(
                            egui::TextEdit::singleline(&mut c.lock_mac)
                                .hint_text("BB:CC:DD:EE:FF:00"),
                        )
                        .changed();
                    ui.end_row();

                    ui.label("HMAC secret");
                    changed |= ui
                        .add(
                            egui::TextEdit::singleline(&mut c.secret)
                                .password(true)
                                .hint_text("(optional but recommended)"),
                        )
                        .changed();
                    ui.end_row();

                    ui.label("UDP port");
                    let mut port_str = c.port.to_string();
                    let resp = ui.add(
                        egui::TextEdit::singleline(&mut port_str).desired_width(80.0),
                    );
                    if resp.changed() {
                        if let Ok(p) = port_str.parse::<u16>() {
                            if p != c.port {
                                c.port = p;
                                changed = true;
                            }
                        }
                    }
                    ui.end_row();
                });

            ui.add_space(10.0);
            ui.separator();
            ui.add_space(8.0);

            // Actions
            let conn = self.connections[idx].clone();
            let mut dispatch: Option<ActionKind> = None;
            let mut want_ping = false;

            ui.horizontal(|ui| {
                let wake = egui::Button::new(egui::RichText::new("Wake").strong())
                    .fill(egui::Color32::from_rgb(60, 110, 60));
                if ui
                    .add_sized([110.0, 36.0], wake)
                    .on_hover_text("Send standard WoL magic packet (broadcast, no HMAC)")
                    .clicked()
                {
                    dispatch = Some(ActionKind::Wake);
                }
                let shutdown = egui::Button::new(egui::RichText::new("Shutdown").strong())
                    .fill(egui::Color32::from_rgb(140, 60, 60));
                if ui
                    .add_sized([110.0, 36.0], shutdown)
                    .on_hover_text("Send HMAC-authenticated shutdown packet to host")
                    .clicked()
                {
                    dispatch = Some(ActionKind::Shutdown);
                }
                let lock = egui::Button::new(egui::RichText::new("Lock").strong())
                    .fill(egui::Color32::from_rgb(70, 70, 130));
                if ui
                    .add_sized([110.0, 36.0], lock)
                    .on_hover_text("Send HMAC-authenticated lock packet to host")
                    .clicked()
                {
                    dispatch = Some(ActionKind::Lock);
                }
                if ui.add_sized([110.0, 36.0], egui::Button::new("Ping")).clicked() {
                    want_ping = true;
                }
            });

            ui.add_space(8.0);

            // Status line
            match self.ping_status.get(&idx) {
                Some(PingResult::Up(rtt)) => {
                    ui.colored_label(
                        egui::Color32::from_rgb(80, 200, 120),
                        format!("● Online — {} ms", rtt),
                    );
                }
                Some(PingResult::Down(e)) => {
                    ui.colored_label(
                        egui::Color32::from_rgb(200, 80, 80),
                        format!("● Offline — {}", e),
                    );
                }
                Some(PingResult::Pending) => {
                    ui.label("· Checking…");
                }
                None => {
                    ui.weak("· not yet checked");
                }
            }

            ui.add_space(10.0);
            ui.separator();
            ui.add_space(6.0);

            // Save / Delete row
            let mut want_save = false;
            let mut want_delete = false;
            ui.horizontal(|ui| {
                let save_btn = egui::Button::new(if self.dirty { "Save" } else { "Saved" });
                if ui
                    .add_enabled(self.dirty, save_btn)
                    .on_hover_text("Write connections.json next to the executable")
                    .clicked()
                {
                    want_save = true;
                }
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    let del = egui::Button::new(egui::RichText::new("Delete").color(egui::Color32::WHITE))
                        .fill(egui::Color32::from_rgb(150, 50, 50));
                    if ui.add(del).on_hover_text("Remove this connection (Save to persist)").clicked() {
                        want_delete = true;
                    }
                });
            });

            if let Some(kind) = dispatch {
                self.spawn_action(conn, kind);
            }
            if want_ping {
                self.spawn_ping(idx);
            }

            if changed {
                self.dirty = true;
            }

            if want_save {
                self.save_to_disk();
                self.dirty = false;
                self.push_log("saved connections.json".into());
            }
            if want_delete {
                self.confirm_delete = Some(idx);
            }
        });

        // Confirmation modal for delete
        if let Some(idx) = self.confirm_delete {
            let name = self
                .connections
                .get(idx)
                .map(|c| c.name.clone())
                .unwrap_or_default();
            let mut close = false;
            let mut do_delete = false;
            egui::Window::new("Confirm delete")
                .collapsible(false)
                .resizable(false)
                .anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0])
                .show(ctx, |ui| {
                    ui.add_space(4.0);
                    ui.label(format!(
                        "Delete '{}'?",
                        if name.is_empty() { "(unnamed)" } else { name.as_str() }
                    ));
                    ui.weak("This is local only — Save to persist removal.");
                    ui.add_space(8.0);
                    ui.horizontal(|ui| {
                        if ui.button("Cancel").clicked() {
                            close = true;
                        }
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            let danger = egui::Button::new(
                                egui::RichText::new("Delete").color(egui::Color32::WHITE),
                            )
                            .fill(egui::Color32::from_rgb(150, 50, 50));
                            if ui.add(danger).clicked() {
                                do_delete = true;
                            }
                        });
                    });
                });

            if do_delete {
                if let Some(c) = self.connections.get(idx).cloned() {
                    self.connections.remove(idx);
                    self.ping_status.clear();
                    match self.selected {
                        Some(s) if s == idx => self.selected = None,
                        Some(s) if s > idx => self.selected = Some(s - 1),
                        _ => {}
                    }
                    self.dirty = true;
                    self.push_log(format!("removed '{}' (Save to persist)", c.name));
                }
                self.confirm_delete = None;
            } else if close {
                self.confirm_delete = None;
            }
        }
    }
}
