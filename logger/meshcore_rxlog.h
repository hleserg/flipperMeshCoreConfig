/*
 * The RX log push (0x88) — the Logger's primary data source.
 *
 * meshcore_c declares this code but does not decode it, so the layout here
 * comes from the firmware itself. MyMesh::logRxRaw() in
 * examples/companion_radio/MyMesh.cpp builds it as:
 *
 *     [0] 0x88               PUSH_CODE_LOG_RX_DATA
 *     [1] (int8_t)(snr * 4)  SNR in quarter-dB
 *     [2] (int8_t)rssi       RSSI in dBm
 *     [3..] raw              the received packet, verbatim
 *
 * Worth knowing: Dispatcher::checkRecv() calls logRxRaw() for *every* raw
 * frame off the radio, with no build flag and no runtime switch, and before
 * the packet is even parsed. So a stock companion build emits this for
 * everything it hears — including packets it cannot decode, which is exactly
 * what a field survey wants.
 *
 * Free of furi so the host tests can cover the decode and the formatting.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** PUSH_CODE_LOG_RX_DATA. */
#define MESHCORE_RXLOG_CODE 0x88u

/** Longest SNR rendering, e.g. "-31.75" plus NUL. */
#define MESHCORE_SNR_LEN 10

typedef struct {
    int8_t snr_q4; /* quarter-dB; divide by 4 for dB */
    int8_t rssi; /* dBm */
    const uint8_t* raw; /* points into the caller's payload buffer */
    size_t raw_len;
} MeshCoreRxLog;

/** Decode one RX log payload. False if it is not one, or is too short. */
bool meshcore_rxlog_parse(const uint8_t* payload, size_t len, MeshCoreRxLog* out);

/** Lowercase hex, no separators. Returns the number of characters written,
 *  and always NUL-terminates. Truncates rather than overflowing `cap`. */
size_t meshcore_hex_encode(const uint8_t* data, size_t len, char* out, size_t cap);

/** Quarter-dB to a decimal string, e.g. 25 -> "6.25", -5 -> "-1.25". Done
 *  without floating point so the value is exact and reproducible. */
void meshcore_rxlog_format_snr(int8_t snr_q4, char* out, size_t cap);
