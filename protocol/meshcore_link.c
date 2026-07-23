#include "meshcore_link.h"

/* Read granularity. The UART layer hands over whatever has arrived, so this
 * only bounds one memcpy, not latency. */
#define MESHCORE_LINK_CHUNK 64u

void meshcore_link_init(MeshCoreLink* link) {
    furi_assert(link);
    link->uart = NULL;
    link->log = NULL;
    mc_rx_init(&link->rx);
}

bool meshcore_link_open(MeshCoreLink* link, MeshCoreLog* log) {
    furi_assert(link);
    furi_assert(!link->uart);

    link->log = log;
    mc_rx_init(&link->rx);

    link->uart = meshcore_uart_open();
    if(!link->uart) {
        meshcore_log_printf(log, "open: USART busy");
        return false;
    }

    meshcore_log_printf(log, "open: %lu 8N1", (unsigned long)MESHCORE_UART_BAUD);
    return true;
}

void meshcore_link_close(MeshCoreLink* link) {
    furi_assert(link);
    if(!link->uart) return;

    meshcore_uart_close(link->uart);
    link->uart = NULL;
    meshcore_log_printf(link->log, "close");
}

bool meshcore_link_is_open(const MeshCoreLink* link) {
    furi_assert(link);
    return link->uart != NULL;
}

void meshcore_link_flush(MeshCoreLink* link) {
    furi_assert(link);
    if(link->uart) meshcore_uart_rx_flush(link->uart);
    mc_rx_init(&link->rx);
}

bool meshcore_link_send(MeshCoreLink* link, const uint8_t* payload, size_t len) {
    furi_assert(link);
    if(!link->uart || len == 0) return false;

    size_t framed = mc_frame_encode(payload, len, link->frame, sizeof(link->frame));
    if(framed == 0) return false;

    meshcore_log_frame(link->log, true, payload, len);
    meshcore_uart_tx(link->uart, link->frame, framed);
    return true;
}

bool meshcore_link_poll(MeshCoreLink* link, mc_event_t* ev, uint32_t timeout_ms) {
    furi_assert(link);
    furi_assert(ev);

    ev->code = MESHCORE_LINK_NO_EVENT;
    if(!link->uart) return false;

    const uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(timeout_ms);

    for(;;) {
        /* Drain everything the assembler already holds before waiting on the
         * UART again — one read can carry several frames. */
        size_t len = 0;
        while(mc_rx_poll(&link->rx, link->payload, sizeof(link->payload), &len)) {
            meshcore_log_frame(link->log, false, link->payload, len);
            if(mc_parse(link->payload, len, ev)) return true;
            /* Code unknown to this build of meshcore_c: logged, then ignored. */
        }

        /* Signed difference so the tick counter wrapping around is harmless. */
        int32_t remaining = (int32_t)(deadline - furi_get_tick());
        if(remaining <= 0) return false;

        uint8_t chunk[MESHCORE_LINK_CHUNK];
        size_t got = meshcore_uart_rx(link->uart, chunk, sizeof(chunk), (uint32_t)remaining);
        if(got) mc_rx_feed(&link->rx, chunk, got);
    }
}

bool meshcore_link_request(
    MeshCoreLink* link,
    const uint8_t* payload,
    size_t len,
    uint8_t want_code,
    mc_event_t* ev,
    uint32_t timeout_ms) {
    furi_assert(link);
    furi_assert(ev);

    ev->code = MESHCORE_LINK_NO_EVENT;
    if(!meshcore_link_send(link, payload, len)) return false;

    const uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(timeout_ms);

    for(;;) {
        int32_t remaining = (int32_t)(deadline - furi_get_tick());
        if(remaining <= 0) {
            ev->code = MESHCORE_LINK_NO_EVENT;
            return false;
        }

        if(!meshcore_link_poll(link, ev, (uint32_t)remaining)) {
            ev->code = MESHCORE_LINK_NO_EVENT;
            return false;
        }

        if(ev->code == want_code) return true;
        if(ev->code == MC_RESP_ERR) return false;

        /* Anything else (a push, or a reply to an earlier command) is not what
         * we asked for — keep waiting until the deadline. */
    }
}

uint32_t meshcore_link_rx_errors(const MeshCoreLink* link) {
    furi_assert(link);
    return link->uart ? meshcore_uart_rx_errors(link->uart) : 0;
}
