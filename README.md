# poweroffd

A production-grade daemon that listens for Wake-on-LAN style magic packets on a custom UDP port and shuts down the system when a valid packet is received. Supports both **Linux** and **Windows**.

## Magic Packet Format

Standard WoL magic packet: 6 bytes of `0xFF` followed by the target MAC address repeated 16 times (102 bytes total). poweroffd extends this with an optional HMAC-SHA256 authentication tag appended after the standard payload (134 bytes total) to prevent unauthorized shutdowns.

## Features

- HMAC-SHA256 packet authentication (optional but recommended)
- MAC address allowlist filtering
- Rate limiting to prevent abuse
- Configurable shutdown delay with cancellation
- Cross-platform: Linux (systemd) and Windows (SCM service)
- Syslog (Linux) / Event Log (Windows) logging
- Privilege dropping after binding socket (Linux)
- PID file and signal handling (Linux)
- Zero external dependencies on Windows (uses Windows CNG for crypto)

## Linux

### Build

```bash
# Dependencies: g++, libcap-dev, libssl-dev
make
sudo make install
```

### Quick Install (Debian/Ubuntu)

```bash
curl -fsSL https://raw.githubusercontent.com/kasaiarashi/poweroffd/main/install.sh | sudo bash
```

### Configuration

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

### Usage

```bash
# Start via systemd
sudo systemctl enable --now poweroffd

# Reload config without restart
sudo systemctl reload poweroffd

# Send a shutdown packet (from another machine)
./poweroff-send <target-ip> <mac> [secret] [port]
```

### Security (Linux)

- Drops root privileges after binding the socket (keeps `CAP_SYS_BOOT`)
- systemd hardening: `ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp=yes`

## Windows

### Build

Requires Visual Studio 2019+ or Build Tools with C++ workload. **No external dependencies** — uses Winsock2 and Windows CNG (BCrypt) for HMAC.

```batch
REM From a Developer Command Prompt:
cd windows
build.bat
```

Or with CMake directly:

```batch
cd windows
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

Or compile manually:

```batch
cd windows
cl /std:c++17 /O2 /EHsc poweroffd-win.cpp /link ws2_32.lib bcrypt.lib advapi32.lib
cl /std:c++17 /O2 /EHsc poweroff-send-win.cpp /link ws2_32.lib bcrypt.lib
```

### Install as Service

```powershell
# Run as Administrator
cd windows
powershell -ExecutionPolicy Bypass -File install.ps1
```

This will:
1. Copy binaries to `%ProgramFiles%\poweroffd\`
2. Create default config at `%ProgramData%\poweroffd\poweroffd.conf`
3. Register as an auto-start Windows service with failure recovery
4. Add a Windows Firewall rule for UDP port 9
5. Add the install directory to system PATH

### Configuration (Windows)

```ini
# %ProgramData%\poweroffd\poweroffd.conf
port = 9
bind = 0.0.0.0
mac = AA:BB:CC:DD:EE:FF
secret = your-hmac-secret-here
delay = 5
```

Note: `user`, `group`, and `pid_file` are Linux-only options and are silently ignored on Windows.

### Usage (Windows)

```batch
REM Start/stop service
net start poweroffd
net stop poweroffd

REM Debug in foreground (console mode)
poweroffd.exe -f -c path\to\poweroffd.conf

REM Uninstall
powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall

REM Send shutdown packet
poweroff-send <target-ip> <mac> [secret] [port]
```

### Service Management

```batch
REM Install with custom config path
poweroffd.exe install -c "C:\path\to\poweroffd.conf"

REM Remove service
poweroffd.exe uninstall
```

### Security (Windows)

- Service runs as `LocalSystem` (has `SE_SHUTDOWN_NAME` privilege)
- Uses `InitiateSystemShutdownEx` with planned shutdown reason code
- Firewall rule is automatically created during install

## Cross-Platform Compatibility

The same `poweroff-send` client (Linux or Windows) can send shutdown packets to any poweroffd daemon regardless of OS. The magic packet format and HMAC authentication are identical. The same `poweroffd.conf` file works on both platforms (Linux-only fields are silently ignored on Windows).

## Security

- **Always set a `secret` in production** — without it, anyone on the network can shut down your machine
- Rate limited to 1 valid packet per 10 seconds per source IP
- HMAC-SHA256 with constant-time comparison prevents timing attacks

## License

MIT
