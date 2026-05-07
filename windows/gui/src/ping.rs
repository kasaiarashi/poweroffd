use std::net::{IpAddr, ToSocketAddrs};
use winping::{Buffer, Pinger};

pub fn ping(host: &str, timeout_ms: u32) -> Result<u32, String> {
    let ip: IpAddr = match host.parse() {
        Ok(ip) => ip,
        Err(_) => format!("{}:0", host)
            .to_socket_addrs()
            .map_err(|e| format!("resolve: {}", e))?
            .next()
            .map(|s| s.ip())
            .ok_or_else(|| "no address resolved".to_string())?,
    };
    let mut pinger = Pinger::new().map_err(|e| e.to_string())?;
    pinger.set_timeout(timeout_ms);
    let mut buf = Buffer::new();
    pinger.send(ip, &mut buf).map_err(|e| e.to_string())
}
