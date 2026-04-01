/*
 * poweroffd — Windows Service port
 *
 * Listens for WoL-style magic packets on a configurable UDP port.
 * When a valid, authenticated packet targeting the configured MAC is
 * received, initiates system shutdown after a configurable delay.
 *
 * Build: cl /std:c++17 /O2 /EHsc poweroffd-win.cpp /link ws2_32.lib bcrypt.lib advapi32.lib
 * License: MIT
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <sddl.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr size_t MAC_LEN             = 6;
static constexpr size_t WOL_HEADER_LEN      = 6;
static constexpr size_t WOL_REPEAT          = 16;
static constexpr size_t WOL_PAYLOAD_LEN     = WOL_HEADER_LEN + MAC_LEN * WOL_REPEAT; // 102
static constexpr size_t HMAC_TAG_LEN        = 32;   // SHA-256
static constexpr size_t MAX_PACKET_LEN      = WOL_PAYLOAD_LEN + HMAC_TAG_LEN;        // 134
static constexpr int    DEFAULT_PORT        = 9;
static constexpr int    DEFAULT_DELAY       = 5;
static constexpr int    RATE_LIMIT_SECS     = 10;

static const char SERVICE_NAME[]    = "poweroffd";
static const char SERVICE_DISPLAY[] = "Power-Off Daemon (WoL-style remote shutdown)";

// Default paths
static std::string get_default_conf_path() {
    char buf[MAX_PATH];
    if (GetEnvironmentVariableA("ProgramData", buf, MAX_PATH) > 0) {
        return std::string(buf) + "\\poweroffd\\poweroffd.conf";
    }
    return "C:\\ProgramData\\poweroffd\\poweroffd.conf";
}

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_reload{false};
static std::atomic<bool> g_shutdown_pending{false};
static std::atomic<int>  g_shutdown_countdown{0};

// Service globals
static SERVICE_STATUS        g_service_status{};
static SERVICE_STATUS_HANDLE g_status_handle = nullptr;
static HANDLE                g_stop_event = nullptr;
static bool                  g_foreground = false;
static std::string           g_conf_path;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    uint16_t                        port   = DEFAULT_PORT;
    std::string                     bind_addr = "0.0.0.0";
    std::array<uint8_t, MAC_LEN>   mac{};
    bool                            mac_set = false;
    std::string                     secret;
    int                             delay  = DEFAULT_DELAY;
    bool                            foreground = false;
};

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

enum LogLevel { LOG_INFO, LOG_WARNING, LOG_ERROR, LOG_CRITICAL };

static HANDLE g_event_log = nullptr;

static void log_open() {
    if (!g_foreground) {
        g_event_log = RegisterEventSourceA(nullptr, SERVICE_NAME);
    }
}

static void log_close() {
    if (g_event_log) {
        DeregisterEventSource(g_event_log);
        g_event_log = nullptr;
    }
}

static void log_msg(LogLevel level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (g_foreground) {
        const char* prefix = "INFO";
        FILE* out = stdout;
        switch (level) {
            case LOG_WARNING:  prefix = "WARN"; break;
            case LOG_ERROR:    prefix = "ERROR"; out = stderr; break;
            case LOG_CRITICAL: prefix = "CRIT"; out = stderr; break;
            default: break;
        }
        fprintf(out, "[%s] %s\n", prefix, buf);
        fflush(out);
        return;
    }

    if (g_event_log) {
        WORD type = EVENTLOG_INFORMATION_TYPE;
        switch (level) {
            case LOG_WARNING:  type = EVENTLOG_WARNING_TYPE; break;
            case LOG_ERROR:    type = EVENTLOG_ERROR_TYPE; break;
            case LOG_CRITICAL: type = EVENTLOG_ERROR_TYPE; break;
            default: break;
        }
        const char* strings[] = { buf };
        ReportEventA(g_event_log, type, 0, 0, nullptr, 1, 0, strings, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool parse_mac(const std::string& s, std::array<uint8_t, MAC_LEN>& out) {
    unsigned int b[MAC_LEN];
    char sep;
    std::istringstream iss(s);
    for (size_t i = 0; i < MAC_LEN; ++i) {
        if (!(iss >> std::hex >> b[i])) return false;
        if (b[i] > 0xFF) return false;
        out[i] = static_cast<uint8_t>(b[i]);
        if (i < MAC_LEN - 1) {
            if (!iss.get(sep) || (sep != ':' && sep != '-')) return false;
        }
    }
    return iss.eof() || iss.peek() == EOF;
}

static std::string mac_to_string(const std::array<uint8_t, MAC_LEN>& m) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    return buf;
}

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ---------------------------------------------------------------------------
// HMAC-SHA256 using Windows CNG (BCrypt)
// ---------------------------------------------------------------------------

static bool compute_hmac_sha256(const void* key, size_t key_len,
                                const void* data, size_t data_len,
                                uint8_t out[32], ULONG* out_len) {
    BCRYPT_ALG_HANDLE  hAlg  = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status)) return false;

    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                              (PUCHAR)key, (ULONG)key_len, 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

    status = BCryptHashData(hHash, (PUCHAR)data, (ULONG)data_len, 0);
    if (!BCRYPT_SUCCESS(status)) goto cleanup;

    ULONG hash_len;
    ULONG result_len;
    status = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_len,
                               sizeof(hash_len), &result_len, 0);
    if (!BCRYPT_SUCCESS(status) || hash_len != 32) goto cleanup;

    status = BCryptFinishHash(hHash, out, 32, 0);
    if (BCRYPT_SUCCESS(status)) {
        *out_len = 32;
        ok = true;
    }

cleanup:
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// Constant-time comparison
static bool secure_compare(const uint8_t* a, const uint8_t* b, size_t len) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

// ---------------------------------------------------------------------------
// Config parser
// ---------------------------------------------------------------------------

static bool load_config(const std::string& path, Config& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) {
        log_msg(LOG_WARNING, "Cannot open config %s", path.c_str());
        return false;
    }

    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) {
            log_msg(LOG_WARNING, "%s:%d: malformed line, skipping", path.c_str(), lineno);
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "port") {
            int p = std::stoi(val);
            if (p < 1 || p > 65535) {
                log_msg(LOG_ERROR, "%s:%d: invalid port %d", path.c_str(), lineno, p);
                return false;
            }
            cfg.port = static_cast<uint16_t>(p);
        } else if (key == "bind") {
            cfg.bind_addr = val;
        } else if (key == "mac") {
            if (!parse_mac(val, cfg.mac)) {
                log_msg(LOG_ERROR, "%s:%d: invalid MAC '%s'", path.c_str(), lineno, val.c_str());
                return false;
            }
            cfg.mac_set = true;
        } else if (key == "secret") {
            cfg.secret = val;
        } else if (key == "delay") {
            cfg.delay = std::stoi(val);
            if (cfg.delay < 0) cfg.delay = 0;
        } else if (key == "user" || key == "group" || key == "pid_file") {
            // Linux-only options — silently ignore for cross-platform config compat
        } else {
            log_msg(LOG_WARNING, "%s:%d: unknown key '%s'", path.c_str(), lineno, key.c_str());
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Packet validation
// ---------------------------------------------------------------------------

struct PacketResult {
    bool valid              = false;
    bool auth_ok            = false;
    std::array<uint8_t, MAC_LEN> target_mac{};
};

static PacketResult validate_packet(const uint8_t* buf, int len, const Config& cfg) {
    PacketResult result;

    if (len < static_cast<int>(WOL_PAYLOAD_LEN)) return result;

    // Verify 6x 0xFF header
    for (size_t i = 0; i < WOL_HEADER_LEN; ++i) {
        if (buf[i] != 0xFF) return result;
    }

    // Extract target MAC
    std::memcpy(result.target_mac.data(), buf + WOL_HEADER_LEN, MAC_LEN);

    // Verify all 16 repetitions match
    for (size_t i = 1; i < WOL_REPEAT; ++i) {
        if (std::memcmp(buf + WOL_HEADER_LEN + i * MAC_LEN,
                        result.target_mac.data(), MAC_LEN) != 0) {
            return result;
        }
    }

    result.valid = true;

    // Check MAC filter
    if (cfg.mac_set && result.target_mac != cfg.mac) {
        result.valid = false;
        return result;
    }

    // HMAC authentication
    if (!cfg.secret.empty()) {
        if (len < static_cast<int>(MAX_PACKET_LEN)) {
            result.auth_ok = false;
            return result;
        }

        uint8_t expected[HMAC_TAG_LEN];
        ULONG out_len = 0;

        if (compute_hmac_sha256(cfg.secret.data(), cfg.secret.size(),
                                buf, WOL_PAYLOAD_LEN, expected, &out_len)) {
            if (out_len == HMAC_TAG_LEN &&
                secure_compare(expected, buf + WOL_PAYLOAD_LEN, HMAC_TAG_LEN)) {
                result.auth_ok = true;
            }
        }
    } else {
        result.auth_ok = true;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Socket setup
// ---------------------------------------------------------------------------

static SOCKET create_socket(const Config& cfg) {
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) {
        log_msg(LOG_ERROR, "socket() failed: %d", WSAGetLastError());
        return INVALID_SOCKET;
    }

    // Set non-blocking
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);

    BOOL optval = TRUE;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&optval, sizeof(optval));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg.port);

    if (inet_pton(AF_INET, cfg.bind_addr.c_str(), &addr.sin_addr) != 1) {
        log_msg(LOG_ERROR, "Invalid bind address '%s'", cfg.bind_addr.c_str());
        closesocket(fd);
        return INVALID_SOCKET;
    }

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        log_msg(LOG_ERROR, "bind(%s:%d) failed: %d",
                cfg.bind_addr.c_str(), cfg.port, WSAGetLastError());
        closesocket(fd);
        return INVALID_SOCKET;
    }

    log_msg(LOG_INFO, "Listening on %s:%d/udp", cfg.bind_addr.c_str(), cfg.port);
    return fd;
}

// ---------------------------------------------------------------------------
// Shutdown execution
// ---------------------------------------------------------------------------

static bool enable_shutdown_privilege() {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        log_msg(LOG_ERROR, "OpenProcessToken failed: %lu", GetLastError());
        return false;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!LookupPrivilegeValueA(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
        log_msg(LOG_ERROR, "LookupPrivilegeValue(SE_SHUTDOWN_NAME) failed: %lu", GetLastError());
        CloseHandle(hToken);
        return false;
    }

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        log_msg(LOG_ERROR, "AdjustTokenPrivileges failed: %lu", GetLastError());
        CloseHandle(hToken);
        return false;
    }

    CloseHandle(hToken);
    return true;
}

static void execute_shutdown() {
    log_msg(LOG_CRITICAL, "Executing system shutdown NOW");

    if (!enable_shutdown_privilege()) {
        log_msg(LOG_ERROR, "Failed to enable shutdown privilege");
        return;
    }

    // InitiateSystemShutdownEx with 0 timeout for immediate shutdown
    if (!InitiateSystemShutdownExA(
            nullptr,                                // local machine
            (LPSTR)"poweroffd: Remote shutdown requested",  // message
            0,                                      // timeout (seconds)
            TRUE,                                   // force close apps
            FALSE,                                  // don't reboot, power off
            SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED)) {
        log_msg(LOG_ERROR, "InitiateSystemShutdownEx failed: %lu — trying ExitWindowsEx",
                GetLastError());

        // Fallback
        if (!ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCE | EWX_POWEROFF,
                           SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED)) {
            log_msg(LOG_ERROR, "ExitWindowsEx also failed: %lu", GetLastError());
        }
    }
}

// ---------------------------------------------------------------------------
// Rate limiting
// ---------------------------------------------------------------------------

class RateLimiter {
public:
    using clock = std::chrono::steady_clock;

    bool allow(uint32_t src_ip) {
        auto now = clock::now();
        auto it = last_seen_.find(src_ip);
        if (it != last_seen_.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
            if (elapsed < RATE_LIMIT_SECS) return false;
        }
        last_seen_[src_ip] = now;
        if (last_seen_.size() > 1024) prune(now);
        return true;
    }

private:
    void prune(clock::time_point now) {
        for (auto it = last_seen_.begin(); it != last_seen_.end(); ) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
            if (age > RATE_LIMIT_SECS * 10)
                it = last_seen_.erase(it);
            else
                ++it;
        }
    }

    std::unordered_map<uint32_t, clock::time_point> last_seen_;
};

// ---------------------------------------------------------------------------
// Main daemon loop
// ---------------------------------------------------------------------------

static void run_daemon(Config& cfg) {
    // Initialize Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_msg(LOG_ERROR, "WSAStartup failed");
        return;
    }

    SOCKET sockfd = create_socket(cfg);
    if (sockfd == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    log_msg(LOG_INFO, "poweroffd ready (port=%d mac=%s hmac=%s delay=%ds)",
            cfg.port,
            cfg.mac_set ? mac_to_string(cfg.mac).c_str() : "ANY",
            cfg.secret.empty() ? "off" : "on",
            cfg.delay);

    RateLimiter limiter;
    uint8_t buf[512];

    while (g_running) {
        // Handle config reload
        if (g_reload.exchange(false)) {
            log_msg(LOG_INFO, "Reloading configuration from %s", g_conf_path.c_str());
            Config new_cfg;
            new_cfg.foreground = cfg.foreground;
            if (load_config(g_conf_path, new_cfg)) {
                cfg.mac     = new_cfg.mac;
                cfg.mac_set = new_cfg.mac_set;
                cfg.secret  = new_cfg.secret;
                cfg.delay   = new_cfg.delay;
                log_msg(LOG_INFO, "Config reloaded (mac=%s hmac=%s delay=%ds)",
                        cfg.mac_set ? mac_to_string(cfg.mac).c_str() : "ANY",
                        cfg.secret.empty() ? "off" : "on",
                        cfg.delay);
            }
        }

        // Handle pending shutdown countdown
        if (g_shutdown_pending) {
            int remaining = g_shutdown_countdown.load();
            if (remaining <= 0) {
                execute_shutdown();
                g_shutdown_pending = false;
                // If we're still running, shutdown failed
                continue;
            } else {
                log_msg(LOG_WARNING, "Shutdown in %d seconds", remaining);

                // Use select with 1-second timeout for countdown
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(sockfd, &readfds);
                struct timeval tv = { 1, 0 };

                int ret = select(0, &readfds, nullptr, nullptr, &tv);
                g_shutdown_countdown.fetch_sub(1);

                if (g_reload.exchange(false)) {
                    log_msg(LOG_WARNING, "Shutdown CANCELLED");
                    g_shutdown_pending = false;
                    Config new_cfg;
                    if (load_config(g_conf_path, new_cfg)) {
                        cfg.mac = new_cfg.mac; cfg.mac_set = new_cfg.mac_set;
                        cfg.secret = new_cfg.secret; cfg.delay = new_cfg.delay;
                    }
                }

                if (ret > 0 && FD_ISSET(sockfd, &readfds)) goto read_packet;
                continue;
            }
        }

        {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sockfd, &readfds);
            struct timeval tv = { 2, 0 }; // 2s timeout

            int ret = select(0, &readfds, nullptr, nullptr, &tv);
            if (ret == SOCKET_ERROR) {
                log_msg(LOG_ERROR, "select() failed: %d", WSAGetLastError());
                break;
            }
            if (ret == 0) continue; // Timeout
        }

    read_packet:
        struct sockaddr_in src_addr{};
        int addr_len = sizeof(src_addr);
        int n = recvfrom(sockfd, (char*)buf, sizeof(buf), 0,
                         reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);

        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) continue;
            log_msg(LOG_ERROR, "recvfrom() failed: %d", err);
            continue;
        }

        char src_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src_addr.sin_addr, src_ip, sizeof(src_ip));

        auto result = validate_packet(buf, n, cfg);

        if (!result.valid) continue;

        if (!result.auth_ok) {
            log_msg(LOG_WARNING, "HMAC auth failed for packet from %s:%d (target %s)",
                    src_ip, ntohs(src_addr.sin_port),
                    mac_to_string(result.target_mac).c_str());
            continue;
        }

        if (!limiter.allow(src_addr.sin_addr.s_addr)) {
            log_msg(LOG_WARNING, "Rate limited packet from %s", src_ip);
            continue;
        }

        log_msg(LOG_INFO, "Valid shutdown packet from %s:%d for MAC %s",
                src_ip, ntohs(src_addr.sin_port),
                mac_to_string(result.target_mac).c_str());

        if (cfg.delay > 0) {
            g_shutdown_pending = true;
            g_shutdown_countdown = cfg.delay;
            log_msg(LOG_CRITICAL, "System shutdown initiated — %d second delay", cfg.delay);
        } else {
            execute_shutdown();
        }
    }

    // Cleanup
    log_msg(LOG_INFO, "poweroffd shutting down");
    closesocket(sockfd);
    WSACleanup();
}

// ---------------------------------------------------------------------------
// Windows Service plumbing
// ---------------------------------------------------------------------------

static DWORD WINAPI service_ctrl_handler(DWORD control, DWORD eventType,
                                          LPVOID eventData, LPVOID context) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            g_running = false;
            g_service_status.dwCurrentState = SERVICE_STOP_PENDING;
            g_service_status.dwWaitHint = 5000;
            SetServiceStatus(g_status_handle, &g_service_status);
            if (g_stop_event) SetEvent(g_stop_event);
            return NO_ERROR;

        case SERVICE_CONTROL_PARAMCHANGE:
            // Use SERVICE_CONTROL_PARAMCHANGE as reload trigger
            g_reload = true;
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI service_main(DWORD argc, LPSTR* argv) {
    g_status_handle = RegisterServiceCtrlHandlerExA(SERVICE_NAME, service_ctrl_handler, nullptr);
    if (!g_status_handle) return;

    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = SERVICE_START_PENDING;
    g_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
                                           SERVICE_ACCEPT_PARAMCHANGE;
    g_service_status.dwWin32ExitCode = 0;
    g_service_status.dwCheckPoint = 0;
    g_service_status.dwWaitHint = 10000;
    SetServiceStatus(g_status_handle, &g_service_status);

    g_stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    // Load config
    Config cfg;
    if (g_conf_path.empty()) g_conf_path = get_default_conf_path();

    log_open();
    log_msg(LOG_INFO, "poweroffd starting (service mode)");

    if (!load_config(g_conf_path, cfg)) {
        log_msg(LOG_ERROR, "Failed to load config from %s", g_conf_path.c_str());
        g_service_status.dwCurrentState = SERVICE_STOPPED;
        g_service_status.dwWin32ExitCode = 1;
        SetServiceStatus(g_status_handle, &g_service_status);
        return;
    }

    if (!cfg.mac_set) {
        log_msg(LOG_WARNING, "No MAC filter set — will accept packets for ANY MAC");
    }
    if (cfg.secret.empty()) {
        log_msg(LOG_WARNING, "No HMAC secret — packets are NOT authenticated (insecure!)");
    }

    // Report running
    g_service_status.dwCurrentState = SERVICE_RUNNING;
    g_service_status.dwWaitHint = 0;
    SetServiceStatus(g_status_handle, &g_service_status);

    // Run the daemon
    run_daemon(cfg);

    // Stopped
    log_msg(LOG_INFO, "poweroffd stopped");
    log_close();

    g_service_status.dwCurrentState = SERVICE_STOPPED;
    g_service_status.dwWin32ExitCode = 0;
    SetServiceStatus(g_status_handle, &g_service_status);

    if (g_stop_event) CloseHandle(g_stop_event);
}

// ---------------------------------------------------------------------------
// Console Ctrl handler (foreground mode)
// ---------------------------------------------------------------------------

static BOOL WINAPI console_ctrl_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            g_running = false;
            return TRUE;
        default:
            return FALSE;
    }
}

// ---------------------------------------------------------------------------
// Service management helpers
// ---------------------------------------------------------------------------

static int cmd_install(const std::string& conf_path) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);

    std::string cmd = std::string("\"") + exe_path + "\"";
    if (!conf_path.empty()) {
        cmd += " -c \"" + conf_path + "\"";
    }

    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        fprintf(stderr, "Error: Cannot open Service Control Manager (run as Administrator)\n");
        return 1;
    }

    SC_HANDLE svc = CreateServiceA(
        scm, SERVICE_NAME, SERVICE_DISPLAY,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        cmd.c_str(),
        nullptr, nullptr, nullptr,
        nullptr,  // LocalSystem
        nullptr);

    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            fprintf(stderr, "Service '%s' already exists. Use 'uninstall' first.\n", SERVICE_NAME);
        } else {
            fprintf(stderr, "CreateService failed: %lu\n", err);
        }
        CloseServiceHandle(scm);
        return 1;
    }

    // Set description
    SERVICE_DESCRIPTIONA desc;
    desc.lpDescription = (LPSTR)"WoL-style remote shutdown daemon. "
        "Listens for magic packets and shuts down the system when a valid packet is received.";
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Configure recovery: restart on failure
    SC_ACTION actions[3] = {
        { SC_ACTION_RESTART, 5000 },  // 1st failure: restart after 5s
        { SC_ACTION_RESTART, 10000 }, // 2nd failure: restart after 10s
        { SC_ACTION_RESTART, 30000 }, // subsequent: restart after 30s
    };
    SERVICE_FAILURE_ACTIONSA sfa{};
    sfa.dwResetPeriod = 86400; // 24h
    sfa.cActions = 3;
    sfa.lpsaActions = actions;
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa);

    printf("Service '%s' installed successfully.\n", SERVICE_NAME);
    printf("\nNext steps:\n");
    printf("  1. Edit %s\n", conf_path.empty() ? get_default_conf_path().c_str() : conf_path.c_str());
    printf("  2. net start %s\n", SERVICE_NAME);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int cmd_uninstall() {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        fprintf(stderr, "Error: Cannot open Service Control Manager (run as Administrator)\n");
        return 1;
    }

    SC_HANDLE svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        fprintf(stderr, "Service '%s' not found.\n", SERVICE_NAME);
        CloseServiceHandle(scm);
        return 1;
    }

    // Stop if running
    SERVICE_STATUS status;
    if (QueryServiceStatus(svc, &status) && status.dwCurrentState != SERVICE_STOPPED) {
        printf("Stopping service...\n");
        ControlService(svc, SERVICE_CONTROL_STOP, &status);
        Sleep(2000);
    }

    if (!DeleteService(svc)) {
        fprintf(stderr, "DeleteService failed: %lu\n", GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }

    printf("Service '%s' uninstalled.\n", SERVICE_NAME);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Service mode (default — called by Windows SCM):\n"
        "  %s                      Run as Windows service\n"
        "\n"
        "Management commands (run as Administrator):\n"
        "  %s install [-c FILE]    Install as Windows service\n"
        "  %s uninstall            Remove Windows service\n"
        "\n"
        "Debug mode:\n"
        "  %s -f [-c FILE]         Run in foreground (console mode)\n"
        "\n"
        "Options:\n"
        "  -c FILE   Config file (default: %%ProgramData%%\\poweroffd\\poweroffd.conf)\n"
        "  -f        Run in foreground (don't register as service)\n"
        "  -h        Show this help\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char* argv[]) {
    g_conf_path = get_default_conf_path();
    bool do_install = false;
    bool do_uninstall = false;

    // Parse args
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            g_conf_path = argv[++i];
        } else if (arg == "-f") {
            g_foreground = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "install") {
            do_install = true;
        } else if (arg == "uninstall") {
            do_uninstall = true;
        }
    }

    if (do_install) return cmd_install(g_conf_path);
    if (do_uninstall) return cmd_uninstall();

    if (g_foreground) {
        // Console/foreground mode
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

        Config cfg;
        cfg.foreground = true;

        log_msg(LOG_INFO, "poweroffd starting in foreground (pid %lu)", GetCurrentProcessId());

        if (!load_config(g_conf_path, cfg)) {
            log_msg(LOG_ERROR, "Failed to load config from %s", g_conf_path.c_str());
            return 1;
        }

        if (!cfg.mac_set) {
            log_msg(LOG_WARNING, "No MAC filter set — will accept packets for ANY MAC");
        }
        if (cfg.secret.empty()) {
            log_msg(LOG_WARNING, "No HMAC secret — packets are NOT authenticated (insecure!)");
        }

        run_daemon(cfg);
        return 0;
    }

    // Default: run as Windows service
    SERVICE_TABLE_ENTRYA dispatch[] = {
        { (LPSTR)SERVICE_NAME, service_main },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherA(dispatch)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            fprintf(stderr, "Not running as a service. Use -f for foreground mode.\n");
            fprintf(stderr, "Run '%s -h' for help.\n", argv[0]);
        } else {
            fprintf(stderr, "StartServiceCtrlDispatcher failed: %lu\n", err);
        }
        return 1;
    }

    return 0;
}
