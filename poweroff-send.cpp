/*
 * poweroff-send — Companion client for poweroffd
 *
 * Sends a WoL-style magic packet (optionally HMAC-authenticated) to trigger
 * remote shutdown on a host running poweroffd.
 *
 * Usage: poweroff-send <host> <mac> [secret] [port]
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/hmac.h>
#include <openssl/evp.h>

static constexpr size_t MAC_LEN         = 6;
static constexpr size_t WOL_HEADER_LEN  = 6;
static constexpr size_t WOL_REPEAT      = 16;
static constexpr size_t WOL_PAYLOAD_LEN = WOL_HEADER_LEN + MAC_LEN * WOL_REPEAT;
static constexpr size_t HMAC_TAG_LEN    = 32;
static constexpr int    DEFAULT_PORT    = 9;

static bool parse_mac(const char* s, std::array<uint8_t, MAC_LEN>& out) {
    unsigned int b[MAC_LEN];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6 &&
        sscanf(s, "%x-%x-%x-%x-%x-%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (b[i] > 0xFF) return false;
        out[i] = static_cast<uint8_t>(b[i]);
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "Usage: %s <host> <mac> [secret] [port]\n", argv[0]);
        fprintf(stderr, "  host   — IP or broadcast address of target\n");
        fprintf(stderr, "  mac    — Target MAC (AA:BB:CC:DD:EE:FF)\n");
        fprintf(stderr, "  secret — HMAC secret (must match poweroffd config)\n");
        fprintf(stderr, "  port   — UDP port (default: %d)\n", DEFAULT_PORT);
        return 1;
    }

    const char* host = argv[1];
    const char* secret = (argc >= 4) ? argv[3] : nullptr;
    int port = (argc >= 5) ? atoi(argv[4]) : DEFAULT_PORT;

    std::array<uint8_t, MAC_LEN> mac{};
    if (!parse_mac(argv[2], mac)) {
        fprintf(stderr, "Error: Invalid MAC address '%s'\n", argv[2]);
        return 1;
    }

    // Build magic packet
    uint8_t packet[WOL_PAYLOAD_LEN + HMAC_TAG_LEN];
    size_t packet_len = WOL_PAYLOAD_LEN;

    // Header: 6x 0xFF
    memset(packet, 0xFF, WOL_HEADER_LEN);

    // Body: MAC repeated 16 times
    for (size_t i = 0; i < WOL_REPEAT; ++i) {
        memcpy(packet + WOL_HEADER_LEN + i * MAC_LEN, mac.data(), MAC_LEN);
    }

    // HMAC tag
    if (secret && strlen(secret) > 0) {
        unsigned int out_len = 0;
        HMAC(EVP_sha256(),
             secret, static_cast<int>(strlen(secret)),
             packet, WOL_PAYLOAD_LEN,
             packet + WOL_PAYLOAD_LEN, &out_len);
        packet_len += HMAC_TAG_LEN;
        printf("HMAC authentication enabled (%u byte tag)\n", out_len);
    }

    // Send
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int broadcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1) {
        fprintf(stderr, "Error: Invalid host '%s'\n", host);
        close(fd);
        return 1;
    }

    ssize_t sent = sendto(fd, packet, packet_len, 0,
                          reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    close(fd);

    if (sent < 0) {
        perror("sendto");
        return 1;
    }

    printf("Sent %zd-byte shutdown packet to %s:%d for MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
           sent, host, port,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}
