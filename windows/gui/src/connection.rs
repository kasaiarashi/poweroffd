use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Connection {
    pub name: String,
    pub host: String,
    #[serde(default)]
    pub broadcast: String,
    #[serde(default)]
    pub wake_mac: String,
    #[serde(default, alias = "mac")]
    pub shutdown_mac: String,
    #[serde(default)]
    pub lock_mac: String,
    #[serde(default)]
    pub unlock_mac: String,
    #[serde(default)]
    pub secret: String,
    #[serde(default = "default_port")]
    pub port: u16,
}

fn default_port() -> u16 {
    9
}

impl Default for Connection {
    fn default() -> Self {
        Self::template()
    }
}

impl Connection {
    pub fn template() -> Self {
        Self {
            name: "New connection".into(),
            host: "192.168.1.10".into(),
            broadcast: "255.255.255.255".into(),
            wake_mac: "AA:BB:CC:DD:EE:FF".into(),
            shutdown_mac: "AA:BB:CC:DD:EE:FF".into(),
            lock_mac: String::new(),
            unlock_mac: String::new(),
            secret: String::new(),
            port: 9,
        }
    }
}

/// Legacy location: next to the executable (not writable under Program Files).
fn legacy_path() -> PathBuf {
    let exe = std::env::current_exe().unwrap_or_else(|_| PathBuf::from("."));
    let dir = exe
        .parent()
        .map(|p| p.to_path_buf())
        .unwrap_or_else(|| PathBuf::from("."));
    dir.join("connections.json")
}

/// Per-user location: %APPDATA%\poweroffd\connections.json
pub fn config_path() -> PathBuf {
    match std::env::var_os("APPDATA") {
        Some(appdata) => PathBuf::from(appdata).join("poweroffd").join("connections.json"),
        None => legacy_path(),
    }
}

pub fn load() -> Vec<Connection> {
    let mut bytes = std::fs::read(config_path()).unwrap_or_default();
    if bytes.is_empty() {
        // Fall back to a file saved next to the exe by older versions.
        bytes = std::fs::read(legacy_path()).unwrap_or_default();
    }
    if bytes.is_empty() {
        return Vec::new();
    }
    serde_json::from_slice(&bytes).unwrap_or_default()
}

pub fn save(list: &[Connection]) -> std::io::Result<()> {
    let path = config_path();
    let json = serde_json::to_vec_pretty(list)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    std::fs::write(path, json)
}
