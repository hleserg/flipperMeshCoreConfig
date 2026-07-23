/*
 * Protocol layer: binds the vendored meshcore_c core to the UART layer.
 *
 * meshcore_c does no I/O — it assembles frames from bytes you feed it and
 * builds command payloads into buffers you own. This file is the only place
 * that connects those two halves to furi_hal_serial.
 *
 * Every call here blocks. Call from a worker thread, never from the GUI
 * thread. See scenes/meshcore_scene_connect.c for the pattern.
 */
#pragma once

#include <furi.h>

#include "meshcore_c/meshcore_companion.h"

#include "../meshcore_log.h"
#include "../uart/meshcore_uart.h"

/* Protocol version this app negotiates: 3 = accept V3 frames (SNR + RSSI),
 * matching what mc_cmd_app_start() advertises. */
#define MESHCORE_LINK_PROTO_VER 3u

/* Name this app reports to the node in APP_START. */
#define MESHCORE_LINK_APP_NAME "MeshCoreCfg"

/* A node that is alive answers well inside this; a node that is not there,
 * or wired TX-to-TX, never will. Kept short so that backing out of a scene
 * does not wait long for the worker to notice. */
#define MESHCORE_LINK_TIMEOUT_MS 1200u

/* ev->code value meaning "no event was decoded" — not a valid wire code. */
#define MESHCORE_LINK_NO_EVENT 0xFFu

typedef struct {
    MeshCoreUart* uart; /* NULL when closed */
    MeshCoreLog* log;
    mc_rx_t rx;
    uint8_t payload[MC_MAX_PAYLOAD]; /* scratch: last assembled payload */
    uint8_t frame[MC_RX_BUFSZ]; /* scratch: outbound framed bytes  */
} MeshCoreLink;

/** Zero the struct. Safe to call on an already-closed link. */
void meshcore_link_init(MeshCoreLink* link);

bool meshcore_link_open(MeshCoreLink* link, MeshCoreLog* log);
void meshcore_link_close(MeshCoreLink* link);
bool meshcore_link_is_open(const MeshCoreLink* link);

/** Drop buffered bytes and reset the frame assembler. */
void meshcore_link_flush(MeshCoreLink* link);

/** Frame `payload` and push it out. False if it would not fit. */
bool meshcore_link_send(MeshCoreLink* link, const uint8_t* payload, size_t len);

/** Pump the UART until one event decodes or the timeout expires. */
bool meshcore_link_poll(MeshCoreLink* link, mc_event_t* ev, uint32_t timeout_ms);

/** Send `payload`, then wait for a reply carrying `want_code`.
 *
 * Unsolicited pushes (0x80+) that arrive meanwhile are logged and skipped.
 * On a node-side failure the call returns false with `ev->code` set to
 * MC_RESP_ERR; on timeout `ev->code` is MESHCORE_LINK_NO_EVENT.
 */
bool meshcore_link_request(
    MeshCoreLink* link,
    const uint8_t* payload,
    size_t len,
    uint8_t want_code,
    mc_event_t* ev,
    uint32_t timeout_ms);

/** Line errors seen since open — non-zero points at baud or wiring problems. */
uint32_t meshcore_link_rx_errors(const MeshCoreLink* link);
