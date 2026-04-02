/*
 * poweroffd — Production-grade Linux daemon
 *
 * Listens for WoL-style magic packets on a configurable UDP port.
 * When a valid, authenticated packet targeting the configured MAC is
 * received, initiates system shutdown after a configurable delay.
 *
 * Build: g++ -std=c++17 -O2 -Wall -Wextra -o poweroffd poweroffd.cpp -lcrypto
 * License: MIT
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/reboot.h>
#include <syslog.h>
#include <unistd.h>
#include <linux/reboot.h>
#include <poll.h>

#include <openssl/hmac.h>
#include <openssl/evp.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr size_t MAC_LEN             = 6;
static constexpr size_t WOL_HEADER_LEN      = 6;   // 6x 0xFF
static constexpr size_t WOL_REPEAT          = 16;
static constexpr size_t WOL_PAYLOAD_LEN     = WOL_HEADER_LEN + MAC_LEN * WOL_REPEAT; // 102
static constexpr size_t HMAC_TAG_LEN        = 32;   // SHA-256
static constexpr size_t MAX_PACKET_LEN      = WOL_PAYLOAD_LEN + HMAC_TAG_LEN;        // 134
static constexpr int    DEFAULT_PORT        = 9;
static constexpr int    DEFAULT_DELAY       = 5;
static constexpr int    RATE_LIMIT_SECS     = 10;
static constexpr char   DEFAULT_PID_FILE[]  = "/run/poweroffd.pid";
static constexpr char   DEFAULT_CONF_FILE[] = "/etc/poweroffd.conf";

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_reload{false};
static std::atomic<bool> g_shutdown_pending{false};
static std::atomic<int>  g_shutdown_countdown{0};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    uint16_t                        port   = DEFAULT_PORT;
    std::string                     bind_addr = "0.0.0.0";
    std::array<uint8_t, MAC_LEN>   mac{};
    bool                            mac_set = false;
    std::array<uint8_t, MAC_LEN>   lock_mac{};
    bool                            lock_mac_set = false;
    std::string                     secret;
    int                             delay  = DEFAULT_DELAY;
    std::string                     user   = "nobody";
    std::string                     group  = "nogroup";
    std::string                     pid_file = DEFAULT_PID_FILE;
    bool                            foreground = false;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void log_msg(int priority, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_msg(int priority, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsyslog(priority, fmt, ap);
    va_end(ap);
}

static bool parse_mac(const std::string& s, std::array<uint8_t, MAC_LEN>& out) {
    // Accept AA:BB:CC:DD:EE:FF or AA-BB-CC-DD-EE-FF
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
// Config parser
// ---------------------------------------------------------------------------

static bool load_config(const std::string& path, Config& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) {
        log_msg(LOG_WARNING, "Cannot open config %s: %s", path.c_str(), strerror(errno));
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
                log_msg(LOG_ERR, "%s:%d: invalid port %d", path.c_str(), lineno, p);
                return false;
            }
            cfg.port = static_cast<uint16_t>(p);
        } else if (key == "bind") {
            cfg.bind_addr = val;
        } else if (key == "mac") {
            if (!parse_mac(val, cfg.mac)) {
                log_msg(LOG_ERR, "%s:%d: invalid MAC '%s'", path.c_str(), lineno, val.c_str());
                return false;
            }
            cfg.mac_set = true;
        } else if (key == "lock_mac") {
            if (!parse_mac(val, cfg.lock_mac)) {
                log_msg(LOG_ERR, "%s:%d: invalid lock_mac '%s'", path.c_str(), lineno, val.c_str());
                return false;
            }
            cfg.lock_mac_set = true;
        } else if (key == "secret") {
            cfg.secret = val;
        } else if (key == "delay") {
            cfg.delay = std::stoi(val);
            if (cfg.delay < 0) cfg.delay = 0;
        } else if (key == "user") {
            cfg.user = val;
        } else if (key == "group") {
            cfg.group = val;
        } else if (key == "pid_file") {
            cfg.pid_file = val;
        } else {
            log_msg(LOG_WARNING, "%s:%d: unknown key '%s'", path.c_str(), lineno, key.c_str());
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Signal handlers
// ---------------------------------------------------------------------------

static void signal_handler(int sig) {
    switch (sig) {
        case SIGTERM:
        case SIGINT:
            g_running = false;
            break;
        case SIGHUP:
            g_reload = true;
            break;
        default:
            break;
    }
}

static void setup_signals() {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGHUP,  &sa, nullptr);

    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
}

// ---------------------------------------------------------------------------
// PID file
// ---------------------------------------------------------------------------

static bool write_pid_file(const std::string& path) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << getpid() << "\n";
    return f.good();
}

static void remove_pid_file(const std::string& path) {
    unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// Privilege management
// ---------------------------------------------------------------------------

static bool drop_privileges(const Config& cfg) {
    // Keep CAP_SYS_BOOT so we can call reboot(2) after dropping root
    if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0) {
        log_msg(LOG_ERR, "prctl(PR_SET_KEEPCAPS) failed: %s", strerror(errno));
        return false;
    }

    struct group* gr = getgrnam(cfg.group.c_str());
    if (!gr) {
        log_msg(LOG_ERR, "Unknown group '%s'", cfg.group.c_str());
        return false;
    }

    struct passwd* pw = getpwnam(cfg.user.c_str());
    if (!pw) {
        log_msg(LOG_ERR, "Unknown user '%s'", cfg.user.c_str());
        return false;
    }

    if (setgid(gr->gr_gid) < 0) {
        log_msg(LOG_ERR, "setgid(%d) failed: %s", gr->gr_gid, strerror(errno));
        return false;
    }

    // Drop supplementary groups
    if (setgroups(0, nullptr) < 0) {
        log_msg(LOG_ERR, "setgroups() failed: %s", strerror(errno));
        return false;
    }

    if (setuid(pw->pw_uid) < 0) {
        log_msg(LOG_ERR, "setuid(%d) failed: %s", pw->pw_uid, strerror(errno));
        return false;
    }

    // Restore CAP_SYS_BOOT in effective set
    cap_t caps = cap_init();
    if (!caps) {
        log_msg(LOG_ERR, "cap_init() failed: %s", strerror(errno));
        return false;
    }

    cap_value_t cap_list[] = { CAP_SYS_BOOT };
    cap_set_flag(caps, CAP_PERMITTED,   1, cap_list, CAP_SET);
    cap_set_flag(caps, CAP_EFFECTIVE,   1, cap_list, CAP_SET);

    if (cap_set_proc(caps) < 0) {
        log_msg(LOG_ERR, "cap_set_proc() failed: %s", strerror(errno));
        cap_free(caps);
        return false;
    }
    cap_free(caps);

    // Lock it down — no further capability changes
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        log_msg(LOG_WARNING, "prctl(PR_SET_NO_NEW_PRIVS) failed: %s", strerror(errno));
    }

    log_msg(LOG_INFO, "Dropped privileges to %s:%s (uid=%d gid=%d)",
            cfg.user.c_str(), cfg.group.c_str(), pw->pw_uid, gr->gr_gid);
    return true;
}

// ---------------------------------------------------------------------------
// Packet validation
// ---------------------------------------------------------------------------

enum PacketAction { ACTION_NONE, ACTION_SHUTDOWN, ACTION_LOCK };

struct PacketResult {
    bool valid              = false;
    bool auth_ok            = false;
    PacketAction action     = ACTION_NONE;
    std::array<uint8_t, MAC_LEN> target_mac{};
};

static PacketResult validate_packet(const uint8_t* buf, ssize_t len, const Config& cfg) {
    PacketResult result;

    // Must be at least a standard WoL payload
    if (len < static_cast<ssize_t>(WOL_PAYLOAD_LEN)) return result;

    // Verify 6x 0xFF header
    for (size_t i = 0; i < WOL_HEADER_LEN; ++i) {
        if (buf[i] != 0xFF) return result;
    }

    // Extract target MAC from first repetition
    std::memcpy(result.target_mac.data(), buf + WOL_HEADER_LEN, MAC_LEN);

    // Verify all 16 repetitions match
    for (size_t i = 1; i < WOL_REPEAT; ++i) {
        if (std::memcmp(buf + WOL_HEADER_LEN + i * MAC_LEN,
                        result.target_mac.data(), MAC_LEN) != 0) {
            return result;
        }
    }

    result.valid = true;

    // Determine action based on which MAC matched
    if (cfg.lock_mac_set && result.target_mac == cfg.lock_mac) {
        result.action = ACTION_LOCK;
    } else if (cfg.mac_set && result.target_mac == cfg.mac) {
        result.action = ACTION_SHUTDOWN;
    } else if (!cfg.mac_set && !cfg.lock_mac_set) {
        result.action = ACTION_SHUTDOWN;
    } else if (!cfg.mac_set && cfg.lock_mac_set) {
        result.action = ACTION_SHUTDOWN;
    } else {
        result.valid = false;
        return result;
    }

    // HMAC authentication
    if (!cfg.secret.empty()) {
        if (len < static_cast<ssize_t>(MAX_PACKET_LEN)) {
            result.auth_ok = false;
            return result;
        }

        unsigned char expected[HMAC_TAG_LEN];
        unsigned int  out_len = 0;

        HMAC(EVP_sha256(),
             cfg.secret.data(), static_cast<int>(cfg.secret.size()),
             buf, WOL_PAYLOAD_LEN,
             expected, &out_len);

        if (out_len == HMAC_TAG_LEN &&
            CRYPTO_memcmp(expected, buf + WOL_PAYLOAD_LEN, HMAC_TAG_LEN) == 0) {
            result.auth_ok = true;
        } else {
            result.auth_ok = false;
        }
    } else {
        result.auth_ok = true;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Socket setup
// ---------------------------------------------------------------------------

static int create_socket(const Config& cfg) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log_msg(LOG_ERR, "socket() failed: %s", strerror(errno));
        return -1;
    }

    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // Allow broadcast reception
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg.port);

    if (inet_pton(AF_INET, cfg.bind_addr.c_str(), &addr.sin_addr) != 1) {
        log_msg(LOG_ERR, "Invalid bind address '%s'", cfg.bind_addr.c_str());
        close(fd);
        return -1;
    }

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_msg(LOG_ERR, "bind(%s:%d) failed: %s",
                cfg.bind_addr.c_str(), cfg.port, strerror(errno));
        close(fd);
        return -1;
    }

    log_msg(LOG_INFO, "Listening on %s:%d/udp", cfg.bind_addr.c_str(), cfg.port);
    return fd;
}

// ---------------------------------------------------------------------------
// Shutdown execution
// ---------------------------------------------------------------------------

static void execute_shutdown() {
    log_msg(LOG_CRIT, "Executing system shutdown NOW");
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    // Fallback if reboot(2) fails (shouldn't if CAP_SYS_BOOT is held)
    log_msg(LOG_ERR, "reboot() failed: %s — falling back to /sbin/poweroff", strerror(errno));
    execl("/sbin/poweroff", "poweroff", nullptr);
    log_msg(LOG_ERR, "execl(/sbin/poweroff) failed: %s", strerror(errno));
}

// ---------------------------------------------------------------------------
// Lock screen
// ---------------------------------------------------------------------------

static void execute_lock() {
    log_msg(LOG_NOTICE, "Locking screen");
    // Try loginctl first (works with systemd-logind)
    int ret = system("loginctl lock-sessions 2>/dev/null");
    if (ret != 0) {
        // Fallback for display managers
        ret = system("dm-tool lock 2>/dev/null");
        if (ret != 0) {
            log_msg(LOG_ERR, "Failed to lock screen (tried loginctl and dm-tool)");
        }
    }
}

// ---------------------------------------------------------------------------
// Daemonize
// ---------------------------------------------------------------------------

static bool daemonize() {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid > 0) _exit(0); // Parent exits

    if (setsid() < 0) return false;

    // Second fork to prevent acquiring a controlling terminal
    pid = fork();
    if (pid < 0) return false;
    if (pid > 0) _exit(0);

    umask(0027);
    if (chdir("/") < 0) return false;

    // Redirect stdio to /dev/null
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }

    return true;
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
        // Evict old entries periodically
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
// Main loop
// ---------------------------------------------------------------------------

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  -c FILE   Config file (default: %s)\n"
        "  -f        Run in foreground (don't daemonize)\n"
        "  -h        Show this help\n",
        prog, DEFAULT_CONF_FILE);
}

int main(int argc, char* argv[]) {
    std::string conf_path = DEFAULT_CONF_FILE;
    Config cfg;

    // Parse command-line args
    int opt;
    while ((opt = getopt(argc, argv, "c:fh")) != -1) {
        switch (opt) {
            case 'c': conf_path = optarg; break;
            case 'f': cfg.foreground = true; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    // Open syslog
    openlog("poweroffd", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    log_msg(LOG_INFO, "poweroffd starting (pid %d)", getpid());

    // Load config
    if (!load_config(conf_path, cfg)) {
        log_msg(LOG_ERR, "Failed to load config from %s", conf_path.c_str());
        return 1;
    }

    if (!cfg.mac_set) {
        log_msg(LOG_WARNING, "No MAC filter set — will accept packets for ANY MAC");
    }

    if (cfg.secret.empty()) {
        log_msg(LOG_WARNING, "No HMAC secret — packets are NOT authenticated (insecure!)");
    }

    // Create socket while still root
    int sockfd = create_socket(cfg);
    if (sockfd < 0) return 1;

    // Daemonize (unless foreground)
    if (!cfg.foreground) {
        if (!daemonize()) {
            log_msg(LOG_ERR, "Daemonization failed");
            close(sockfd);
            return 1;
        }
    }

    // Write PID file
    if (!write_pid_file(cfg.pid_file)) {
        log_msg(LOG_ERR, "Failed to write PID file %s", cfg.pid_file.c_str());
    }

    // Setup signals
    setup_signals();

    // Drop privileges (keep CAP_SYS_BOOT)
    if (getuid() == 0) {
        if (!drop_privileges(cfg)) {
            log_msg(LOG_ERR, "Failed to drop privileges — exiting for safety");
            remove_pid_file(cfg.pid_file);
            close(sockfd);
            return 1;
        }
    } else {
        log_msg(LOG_WARNING, "Not running as root — cannot drop privileges or guarantee shutdown capability");
    }

    log_msg(LOG_INFO, "poweroffd ready (port=%d mac=%s lock_mac=%s hmac=%s delay=%ds)",
            cfg.port,
            cfg.mac_set ? mac_to_string(cfg.mac).c_str() : "ANY",
            cfg.lock_mac_set ? mac_to_string(cfg.lock_mac).c_str() : "off",
            cfg.secret.empty() ? "off" : "on",
            cfg.delay);

    // Main event loop
    RateLimiter limiter;
    uint8_t buf[512]; // Generous buffer
    struct pollfd pfd{};
    pfd.fd = sockfd;
    pfd.events = POLLIN;

    while (g_running) {
        // Handle config reload
        if (g_reload.exchange(false)) {
            log_msg(LOG_INFO, "Reloading configuration from %s", conf_path.c_str());
            Config new_cfg;
            new_cfg.foreground = cfg.foreground;
            new_cfg.pid_file = cfg.pid_file;
            if (load_config(conf_path, new_cfg)) {
                // Can reload: mac, lock_mac, secret, delay. Cannot change port/bind without restart.
                cfg.mac          = new_cfg.mac;
                cfg.mac_set      = new_cfg.mac_set;
                cfg.lock_mac     = new_cfg.lock_mac;
                cfg.lock_mac_set = new_cfg.lock_mac_set;
                cfg.secret       = new_cfg.secret;
                cfg.delay        = new_cfg.delay;
                log_msg(LOG_INFO, "Config reloaded (mac=%s lock_mac=%s hmac=%s delay=%ds)",
                        cfg.mac_set ? mac_to_string(cfg.mac).c_str() : "ANY",
                        cfg.lock_mac_set ? mac_to_string(cfg.lock_mac).c_str() : "off",
                        cfg.secret.empty() ? "off" : "on",
                        cfg.delay);
            }
        }

        // Handle pending shutdown countdown
        if (g_shutdown_pending) {
            int remaining = g_shutdown_countdown.load();
            if (remaining <= 0) {
                execute_shutdown();
                // If we get here, shutdown failed
                g_shutdown_pending = false;
            } else {
                log_msg(LOG_WARNING, "Shutdown in %d seconds (send SIGHUP to cancel)", remaining);
                // Use 1-second poll for countdown
                int ret = poll(&pfd, 1, 1000);
                g_shutdown_countdown.fetch_sub(1);

                if (g_reload.exchange(false)) {
                    log_msg(LOG_WARNING, "Shutdown CANCELLED by SIGHUP");
                    g_shutdown_pending = false;
                    // Still reload config
                    Config new_cfg;
                    if (load_config(conf_path, new_cfg)) {
                        cfg.mac = new_cfg.mac; cfg.mac_set = new_cfg.mac_set;
                        cfg.lock_mac = new_cfg.lock_mac; cfg.lock_mac_set = new_cfg.lock_mac_set;
                        cfg.secret = new_cfg.secret; cfg.delay = new_cfg.delay;
                    }
                }

                if (ret > 0 && (pfd.revents & POLLIN)) goto read_packet;
                continue;
            }
        }

        {
            int ret = poll(&pfd, 1, 2000); // 2s timeout for signal checks
            if (ret < 0) {
                if (errno == EINTR) continue;
                log_msg(LOG_ERR, "poll() failed: %s", strerror(errno));
                break;
            }
            if (ret == 0) continue; // Timeout
        }

    read_packet:
        struct sockaddr_in src_addr{};
        socklen_t addr_len = sizeof(src_addr);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0,
                             reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            log_msg(LOG_ERR, "recvfrom() failed: %s", strerror(errno));
            continue;
        }

        char src_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src_addr.sin_addr, src_ip, sizeof(src_ip));

        // Validate magic packet
        auto result = validate_packet(buf, n, cfg);

        if (!result.valid) {
            // Not a valid WoL packet — silently drop (don't log to avoid noise)
            continue;
        }

        if (!result.auth_ok) {
            log_msg(LOG_WARNING, "HMAC auth failed for packet from %s:%d (target %s)",
                    src_ip, ntohs(src_addr.sin_port),
                    mac_to_string(result.target_mac).c_str());
            continue;
        }

        // Rate limit
        if (!limiter.allow(src_addr.sin_addr.s_addr)) {
            log_msg(LOG_WARNING, "Rate limited packet from %s", src_ip);
            continue;
        }

        log_msg(LOG_NOTICE, "Valid %s packet from %s:%d for MAC %s",
                result.action == ACTION_LOCK ? "lock" : "shutdown",
                src_ip, ntohs(src_addr.sin_port),
                mac_to_string(result.target_mac).c_str());

        if (result.action == ACTION_LOCK) {
            execute_lock();
        } else if (cfg.delay > 0) {
            g_shutdown_pending = true;
            g_shutdown_countdown = cfg.delay;
            log_msg(LOG_CRIT, "System shutdown initiated — %d second delay (SIGHUP to cancel)",
                    cfg.delay);
        } else {
            execute_shutdown();
        }
    }

    // Cleanup
    log_msg(LOG_INFO, "poweroffd shutting down");
    close(sockfd);
    remove_pid_file(cfg.pid_file);
    closelog();
    return 0;
}
