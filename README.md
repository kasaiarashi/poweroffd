# poweroffd

A production-grade Linux daemon that listens for Wake-on-LAN style magic packets on a custom UDP port and shuts down the system when a valid packet is received.

## Magic Packet Format

Standard WoL magic packet: 6 bytes of `0xFF` followed by the target MAC address repeated 16 times (102 bytes total). poweroffd extends this with an optional HMAC-SHA256 authentication tag appended after the standard payload (134 bytes total) to prevent unauthorized shutdowns.

## Features

- Runs as a proper systemd daemon (sd_notify ready)
- HMAC-SHA256 packet authentication (optional but recommended)
- MAC address allowlist filtering
- Rate limiting to prevent abuse
- Privilege dropping after binding socket
- PID file, syslog logging
- Graceful signal handling (SIGTERM, SIGINT, SIGHUP for config reload)
- Configurable via `/etc/poweroffd.conf`
- Shutdown delay with cancellation window

## Build

```bash
make
sudo make install
```

## Configuration

```ini
# /etc/poweroffd.conf
port = 9
bind = 0.0.0.0
mac = AA:BB:CC:DD:EE:FF
secret = your-hmac-secret-here
delay = 5
user = nobody
group = nogroup
```

## Usage

```bash
# Start via systemd
sudo systemctl enable --now poweroffd

# Send a shutdown packet (from another machine)
./poweroff-send <target-ip> <mac> [secret]
```

## Security

- Always set a `secret` in production — without it, anyone on the network can shut down your machine
- The daemon drops root privileges after binding the socket (keeps CAP_SYS_BOOT for shutdown)
- Rate limited to 1 valid packet per 10 seconds
