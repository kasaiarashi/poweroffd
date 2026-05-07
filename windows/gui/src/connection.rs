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
            secret: String::new(),
            port: 9,
        }
    }
}

pub fn config_path() -> PathBuf {
    let exe = std::env::current_exe().unwrap_or_else(|_| PathBuf::from("."));
    let dir = exe
        .parent()
        .map(|p| p.to_path_buf())
        .unwrap_or_else(|| PathBuf::from("."));
    dir.join("connections.json")
}

pub fn load() -> Vec<Connection> {
    let path = config_path();
    let bytes = match std::fs::read(&path) {
        Ok(b) => b,
        Err(_) => return Vec::new(),
    };
    if bytes.is_empty() {
        return Vec::new();
    }
    serde_json::from_slice(&bytes).unwrap_or_default()
}

pub fn save(list: &[Connection]) -> std::io::Result<()> {
    let path = config_path();
    let json = serde_json::to_vec_pretty(list)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
    std::fs::write(path, json)
}
