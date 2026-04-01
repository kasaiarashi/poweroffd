/*
 * poweroff-send — Windows companion client for poweroffd
 *
 * Sends a WoL-style magic packet (optionally HMAC-authenticated) to trigger
 * remote shutdown on a host running poweroffd.
 *
 * Build: cl /std:c++17 /O2 /EHsc poweroff-send-win.cpp /link ws2_32.lib bcrypt.lib
 * License: MIT
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

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

    status = BCryptFinishHash(hHash, out, 32, 0);
    if (BCRYPT_SUCCESS(status)) {
        *out_len = 32;
        ok = true;
    }

cleanup:
    if (hHash) BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
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

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    const char* host = argv[1];
    const char* secret = (argc >= 4) ? argv[3] : nullptr;
    int port = (argc >= 5) ? atoi(argv[4]) : DEFAULT_PORT;

    std::array<uint8_t, MAC_LEN> mac{};
    if (!parse_mac(argv[2], mac)) {
        fprintf(stderr, "Error: Invalid MAC address '%s'\n", argv[2]);
        WSACleanup();
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
        ULONG out_len = 0;
        if (compute_hmac_sha256(secret, strlen(secret),
                                packet, WOL_PAYLOAD_LEN,
                                packet + WOL_PAYLOAD_LEN, &out_len)) {
            packet_len += HMAC_TAG_LEN;
            printf("HMAC authentication enabled (%lu byte tag)\n", out_len);
        } else {
            fprintf(stderr, "Error: HMAC computation failed\n");
            WSACleanup();
            return 1;
        }
    }

    // Send
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    BOOL broadcast = TRUE;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1) {
        fprintf(stderr, "Error: Invalid host '%s'\n", host);
        closesocket(fd);
        WSACleanup();
        return 1;
    }

    int sent = sendto(fd, (const char*)packet, (int)packet_len, 0,
                      reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    closesocket(fd);
    WSACleanup();

    if (sent == SOCKET_ERROR) {
        fprintf(stderr, "sendto failed: %d\n", WSAGetLastError());
        return 1;
    }

    printf("Sent %d-byte shutdown packet to %s:%d for MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
           sent, host, port,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}
