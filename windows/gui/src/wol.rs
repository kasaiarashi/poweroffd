use hmac::{Hmac, Mac};
use sha2::Sha256;
use std::net::{SocketAddr, ToSocketAddrs, UdpSocket};

const WOL_HEADER_LEN: usize = 6;
const WOL_REPEAT: usize = 16;
const MAC_LEN: usize = 6;
const WOL_PAYLOAD_LEN: usize = WOL_HEADER_LEN + MAC_LEN * WOL_REPEAT;

type HmacSha256 = Hmac<Sha256>;

pub fn parse_mac(s: &str) -> Result<[u8; 6], String> {
    let parts: Vec<&str> = s.split(|c| c == ':' || c == '-').collect();
    if parts.len() != 6 {
        return Err(format!("invalid MAC '{}': expected 6 octets", s));
    }
    let mut out = [0u8; 6];
    for (i, p) in parts.iter().enumerate() {
        out[i] = u8::from_str_radix(p.trim(), 16)
            .map_err(|e| format!("MAC octet {}: {}", i + 1, e))?;
    }
    Ok(out)
}

pub fn build_packet(mac: [u8; 6], secret: &str) -> Vec<u8> {
    let mut packet = Vec::with_capacity(WOL_PAYLOAD_LEN + 32);
    packet.extend_from_slice(&[0xFFu8; WOL_HEADER_LEN]);
    for _ in 0..WOL_REPEAT {
        packet.extend_from_slice(&mac);
    }
    if !secret.is_empty() {
        let mut hm = HmacSha256::new_from_slice(secret.as_bytes())
            .expect("HMAC accepts any key length");
        hm.update(&packet);
        let tag = hm.finalize().into_bytes();
        packet.extend_from_slice(&tag);
    }
    packet
}

pub fn send(target: &str, port: u16, packet: &[u8], broadcast: bool) -> std::io::Result<SocketAddr> {
    let sock = UdpSocket::bind("0.0.0.0:0")?;
    if broadcast {
        sock.set_broadcast(true)?;
    }
    let addr_str = format!("{}:{}", target, port);
    let addr: SocketAddr = addr_str
        .to_socket_addrs()?
        .next()
        .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::InvalidInput, "no address resolved"))?;
    sock.send_to(packet, addr)?;
    Ok(addr)
}
