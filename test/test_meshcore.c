/*
 * Host tests for the protocol layer.
 *
 * These exist because the Flipper cannot tell us whether we understood the
 * MeshCore wire format — only a node can, and one is not always on the bench.
 * Everything here runs on a PC in milliseconds and covers the two things that
 * are expensive to get wrong: the frame layout, and the request/reply logic in
 * meshcore_link.c.
 *
 * The vendored library is treated as the thing under test too: if upstream
 * ever changes a field offset, these fail loudly instead of silently
 * misreading a node.
 */
#include <furi.h>

#include "../messenger/meshcore_contacts.h"
#include "../messenger/meshcore_messages.h"
#include "../protocol/meshcore_link.h"
#include "../protocol/meshcore_route.h"
#include "fakes.h"

/* ======================================================================== *
 *  Tiny test harness
 * ======================================================================== */
static int g_checks;
static int g_failures;
static const char* g_section = "";

static void section(const char* name) {
    g_section = name;
    printf("\n-- %s\n", name);
}

#define CHECK(cond, ...)                               \
    do {                                               \
        g_checks++;                                    \
        if(!(cond)) {                                  \
            g_failures++;                              \
            printf("   FAIL [%s] line %d: ", g_section, __LINE__); \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
        }                                              \
    } while(0)

#define CHECK_EQ_U32(got, want, label)                                        \
    CHECK((uint32_t)(got) == (uint32_t)(want), "%s: got %lu, want %lu", label, \
          (unsigned long)(got), (unsigned long)(want))

#define CHECK_EQ_STR(got, want, label) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", label, (got), (want))

static void put_u32_le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ======================================================================== *
 *  Framing
 * ======================================================================== */
static void test_frame_lead_byte(void) {
    section("framing: lead byte and length");

    const uint8_t payload[] = {MC_CMD_APP_START, 0x03};
    uint8_t frame[16];
    size_t n = mc_frame_encode(payload, sizeof(payload), frame, sizeof(frame));

    CHECK_EQ_U32(n, sizeof(payload) + 3, "framed length");

    /* The whole task hinges on this byte. The firmware's writeFrame() emits
     * '>' and checkRecvFrame() waits for '<', both from the node's side, so
     * what we transmit must be led by '<' (0x3C). If this ever flips, nothing
     * works and the failure looks like a dead node. */
    CHECK(frame[0] == 0x3C, "app->radio lead byte: got 0x%02X, want 0x3C ('<')", frame[0]);
    CHECK(frame[0] == MC_FRAME_APP_TO_RADIO, "lead byte should equal MC_FRAME_APP_TO_RADIO");
    CHECK(MC_FRAME_RADIO_TO_APP == 0x3E, "radio->app lead byte should be 0x3E ('>')");

    /* Length is little-endian, and excludes the 3-byte header. */
    CHECK_EQ_U32(frame[1] | (frame[2] << 8), sizeof(payload), "length field");
    CHECK(memcmp(frame + 3, payload, sizeof(payload)) == 0, "payload copied verbatim");
}

static void test_frame_roundtrip(void) {
    section("framing: encode -> feed -> poll round trip");

    const uint8_t payload[] = {MC_RESP_SELF_INFO, 1, 2, 3, 4, 5};
    uint8_t frame[32];
    size_t framed = mc_frame_encode(payload, sizeof(payload), frame, sizeof(frame));

    mc_rx_t rx;
    mc_rx_init(&rx);
    CHECK_EQ_U32(mc_rx_feed(&rx, frame, framed), framed, "bytes accepted");

    uint8_t out[MC_MAX_PAYLOAD];
    size_t out_len = 0;
    CHECK(mc_rx_poll(&rx, out, sizeof(out), &out_len) == 1, "one frame should be ready");
    CHECK_EQ_U32(out_len, sizeof(payload), "payload length");
    CHECK(memcmp(out, payload, sizeof(payload)) == 0, "payload survives the round trip");
    CHECK(mc_rx_poll(&rx, out, sizeof(out), &out_len) == 0, "no second frame");
}

static void test_frame_fragmented(void) {
    section("framing: frame split across reads");

    const uint8_t payload[] = {MC_RESP_OK, 0xAA, 0xBB, 0xCC};
    uint8_t frame[32];
    size_t framed = mc_frame_encode(payload, sizeof(payload), frame, sizeof(frame));

    mc_rx_t rx;
    mc_rx_init(&rx);
    uint8_t out[MC_MAX_PAYLOAD];
    size_t out_len = 0;

    /* One byte at a time: a real UART splits frames wherever it likes. */
    for(size_t i = 0; i + 1 < framed; i++) {
        mc_rx_feed(&rx, frame + i, 1);
        CHECK(mc_rx_poll(&rx, out, sizeof(out), &out_len) == 0, "incomplete frame must not parse");
    }
    mc_rx_feed(&rx, frame + framed - 1, 1);
    CHECK(mc_rx_poll(&rx, out, sizeof(out), &out_len) == 1, "last byte completes the frame");
    CHECK(memcmp(out, payload, sizeof(payload)) == 0, "reassembled payload matches");
}

static void test_frame_two_in_one_read(void) {
    section("framing: two frames in one read");

    const uint8_t a[] = {MC_RESP_OK};
    const uint8_t b[] = {MC_RESP_ERR, 0x07};
    uint8_t buf[32];
    size_t n = mc_frame_encode(a, sizeof(a), buf, sizeof(buf));
    n += mc_frame_encode(b, sizeof(b), buf + n, sizeof(buf) - n);

    mc_rx_t rx;
    mc_rx_init(&rx);
    mc_rx_feed(&rx, buf, n);

    uint8_t out[MC_MAX_PAYLOAD];
    size_t out_len = 0;
    CHECK(mc_rx_poll(&rx, out, sizeof(out), &out_len) == 1, "first frame");
    CHECK(out[0] == MC_RESP_OK, "first frame is OK");
    CHECK(mc_rx_poll(&rx, out, sizeof(out), &out_len) == 1, "second frame");
    CHECK(out[0] == MC_RESP_ERR, "second frame is ERR");
    CHECK(mc_rx_poll(&rx, out, sizeof(out), &out_len) == 0, "and nothing more");
}

static void test_frame_resync_after_garbage(void) {
    section("framing: resync past line noise");

    const uint8_t payload[] = {MC_RESP_OK};
    uint8_t buf[32];
    /* Noise first — what a wrong baud rate or a half-connected wire produces. */
    buf[0] = 0xAA;
    buf[1] = 0x00;
    buf[2] = 0xFF;
    size_t n = 3 + mc_frame_encode(payload, sizeof(payload), buf + 3, sizeof(buf) - 3);

    mc_rx_t rx;
    mc_rx_init(&rx);
    mc_rx_feed(&rx, buf, n);

    uint8_t out[MC_MAX_PAYLOAD];
    size_t out_len = 0;
    CHECK(mc_rx_poll(&rx, out, sizeof(out), &out_len) == 1, "valid frame found after garbage");
    CHECK(out[0] == MC_RESP_OK, "and it decodes correctly");
}

/* ======================================================================== *
 *  Command builders
 * ======================================================================== */
static void test_cmd_app_start(void) {
    section("commands: APP_START");

    uint8_t out[64];
    size_t n = mc_cmd_app_start(out, sizeof(out), "MeshCoreCfg");

    CHECK_EQ_U32(n, 1 + 1 + 6 + 11, "payload length");
    CHECK_EQ_U32(out[0], MC_CMD_APP_START, "command code");
    CHECK_EQ_U32(out[1], 3, "app protocol version");
    CHECK_EQ_U32(out[1], MESHCORE_LINK_PROTO_VER, "app version matches what the link advertises");
    for(int i = 2; i < 8; i++) CHECK_EQ_U32(out[i], 0, "reserved byte");
    CHECK(memcmp(out + 8, "MeshCoreCfg", 11) == 0, "app name");
}

static void test_cmd_set_radio_params(void) {
    section("commands: SET_RADIO_PARAMS");

    uint8_t out[32];
    /* 869.525 MHz is expressed in kHz on the wire — the header calls the
     * argument freq_hz_x1000, which is the same thing said awkwardly. */
    size_t n = mc_cmd_set_radio_params(out, sizeof(out), 869525, 250, 10, 5);

    CHECK_EQ_U32(n, 11, "payload length");
    CHECK_EQ_U32(out[0], MC_CMD_SET_RADIO_PARAMS, "command code");
    CHECK_EQ_U32((uint32_t)out[1] | ((uint32_t)out[2] << 8) | ((uint32_t)out[3] << 16) |
                     ((uint32_t)out[4] << 24),
                 869525, "frequency, kHz, little-endian");
    CHECK_EQ_U32((uint32_t)out[5] | ((uint32_t)out[6] << 8) | ((uint32_t)out[7] << 16) |
                     ((uint32_t)out[8] << 24),
                 250, "bandwidth");
    CHECK_EQ_U32(out[9], 10, "spreading factor");
    CHECK_EQ_U32(out[10], 5, "coding rate");
}

static void test_cmd_misc(void) {
    section("commands: SET_ADVERT_NAME / SEND_SELF_ADVERT / SET_TX_POWER");

    uint8_t out[64];

    size_t n = mc_cmd_set_advert_name(out, sizeof(out), "greyrock");
    CHECK_EQ_U32(n, 1 + 8, "advert name length");
    CHECK_EQ_U32(out[0], MC_CMD_SET_ADVERT_NAME, "advert name command code");
    CHECK(memcmp(out + 1, "greyrock", 8) == 0, "advert name bytes");

    n = mc_cmd_send_self_advert(out, sizeof(out), MC_ADVERT_FLOOD);
    CHECK_EQ_U32(n, 2, "self advert length");
    CHECK_EQ_U32(out[0], MC_CMD_SEND_SELF_ADVERT, "self advert command code");
    CHECK_EQ_U32(out[1], MC_ADVERT_FLOOD, "flood mode");

    n = mc_cmd_set_tx_power(out, sizeof(out), 22);
    CHECK_EQ_U32(n, 5, "tx power length");
    CHECK_EQ_U32(out[0], MC_CMD_SET_TX_POWER, "tx power command code");
    CHECK_EQ_U32(out[1], 22, "tx power dBm");
}

/* ======================================================================== *
 *  Response parsers
 * ======================================================================== */
static void test_parse_self_info(void) {
    section("parsing: SELF_INFO (the config scene_connect reads)");

    uint8_t p[128];
    memset(p, 0, sizeof(p));
    size_t i = 0;
    p[i++] = MC_RESP_SELF_INFO;

    uint8_t* b = p + 1; /* body starts after the response code */
    b[0] = 1; /* type          */
    b[1] = 22; /* tx_power      */
    b[2] = 30; /* max_tx_power  */
    /* b[3..34] public key, b[35..42] lat/lon — left zero */
    b[43] = 0; /* multi_acks       */
    b[44] = 0; /* adv_loc_policy   */
    b[45] = 0; /* telemetry_mode   */
    b[46] = 0; /* manual_add       */
    put_u32_le(b + 47, 869525); /* radio_freq, kHz */
    put_u32_le(b + 51, 250); /* radio_bw        */
    b[55] = 10; /* sf */
    b[56] = 5; /* cr */
    memcpy(b + 57, "greyrock-1", 10);
    size_t body_len = 57 + 10;

    mc_event_t ev;
    CHECK(mc_parse(p, 1 + body_len, &ev) == 1, "SELF_INFO should parse");
    CHECK_EQ_U32(ev.code, MC_RESP_SELF_INFO, "event code");
    CHECK_EQ_U32(ev.u.self_info.tx_power, 22, "tx_power");
    CHECK_EQ_U32(ev.u.self_info.max_tx_power, 30, "max_tx_power");
    CHECK_EQ_U32(ev.u.self_info.radio_freq, 869525, "radio_freq");
    CHECK_EQ_U32(ev.u.self_info.radio_bw, 250, "radio_bw");
    CHECK_EQ_U32(ev.u.self_info.radio_sf, 10, "radio_sf");
    CHECK_EQ_U32(ev.u.self_info.radio_cr, 5, "radio_cr");
    CHECK_EQ_STR(ev.u.self_info.name, "greyrock-1", "node name");

    UNUSED(i);
}

static void test_parse_device_info(void) {
    section("parsing: DEVICE_INFO (model and firmware version)");

    uint8_t p[256];
    memset(p, 0, sizeof(p));
    p[0] = MC_RESP_DEVICE_INFO;

    uint8_t* b = p + 1;
    b[0] = 13; /* fw_ver */
    b[1] = 100; /* max_contacts / 2 */
    b[2] = 20; /* max_channels */
    put_u32_le(b + 3, 123456); /* ble_pin */
    memcpy(b + 7, "09-05-2026", 10); /* build_date, 12-byte field */
    memcpy(b + 19, "Heltec_T114", 11); /* model, 40-byte field     */
    memcpy(b + 59, "v1.12.0", 7); /* ver, 20-byte field       */
    size_t body_len = 79;

    mc_event_t ev;
    CHECK(mc_parse(p, 1 + body_len, &ev) == 1, "DEVICE_INFO should parse");
    CHECK_EQ_U32(ev.u.device_info.fw_ver, 13, "fw_ver");
    CHECK_EQ_U32(ev.u.device_info.max_contacts, 200, "max_contacts is doubled on the wire");
    CHECK_EQ_U32(ev.u.device_info.max_channels, 20, "max_channels");
    CHECK_EQ_STR(ev.u.device_info.build_date, "09-05-2026", "build_date");
    CHECK_EQ_STR(ev.u.device_info.model, "Heltec_T114", "model");
    CHECK_EQ_STR(ev.u.device_info.ver, "v1.12.0", "firmware version string");
}

static void test_parse_contact(void) {
    section("parsing: CONTACT (needed by the messenger contact list)");

    uint8_t p[256];
    memset(p, 0, sizeof(p));
    p[0] = MC_RESP_CONTACT;

    uint8_t* b = p + 1;
    for(int i = 0; i < 32; i++) b[i] = (uint8_t)(0xA0 + i); /* public key */
    b[32] = 1; /* type          */
    b[33] = 0; /* flags         */
    b[34] = 2; /* out_path_len  */
    /* b[35..98] out_path (64 bytes) */
    memcpy(b + 99, "node-bravo", 10); /* adv_name, 32-byte field */
    put_u32_le(b + 131, 1750000000); /* last_advert */
    put_u32_le(b + 135, 0); /* adv_lat     */
    put_u32_le(b + 139, 0); /* adv_lon     */
    put_u32_le(b + 143, 1750000042); /* lastmod     */
    size_t body_len = 147;

    mc_event_t ev;
    CHECK(mc_parse(p, 1 + body_len, &ev) == 1, "CONTACT should parse");
    CHECK_EQ_STR(ev.u.contact.adv_name, "node-bravo", "contact name");
    CHECK_EQ_U32(ev.u.contact.last_advert, 1750000000, "last_advert (drives 'last seen')");
    CHECK_EQ_U32(ev.u.contact.lastmod, 1750000042, "lastmod");
    CHECK_EQ_U32(ev.u.contact.out_path_len, 2, "out_path_len");
    CHECK_EQ_U32(ev.u.contact.public_key[0], 0xA0, "public key first byte");
    CHECK_EQ_U32(ev.u.contact.public_key[31], 0xBF, "public key last byte");

    /* A contact record is 147 bytes plus the code byte — comfortably inside
     * MC_MAX_PAYLOAD, so the assembler never has to drop one. */
    CHECK(1 + body_len <= MC_MAX_PAYLOAD, "contact frame fits the RX buffer");
}

/* ======================================================================== *
 *  Link layer
 * ======================================================================== */
static void link_begin(MeshCoreLink* link) {
    fake_clock_reset();
    fake_uart_reset();
    fake_log_reset();
    meshcore_link_init(link);
    assert(meshcore_link_open(link, fake_log_instance()));
}

/* Queue a reply as it would arrive on the wire: framed, radio->app. */
static void queue_reply(const uint8_t* payload, size_t len) {
    uint8_t frame[MC_RX_BUFSZ];
    size_t n = mc_frame_encode(payload, len, frame, sizeof(frame));
    assert(n > 0);
    frame[0] = MC_FRAME_RADIO_TO_APP; /* the node leads with '>' */
    fake_uart_push_rx(frame, n);
}

static void test_link_send(void) {
    section("link: send frames a payload correctly");

    MeshCoreLink link;
    link_begin(&link);

    const uint8_t payload[] = {MC_CMD_GET_DEVICE_TIME};
    CHECK(meshcore_link_send(&link, payload, sizeof(payload)), "send should succeed");

    CHECK_EQ_U32(fake_uart_tx_len(), 4, "3 header bytes + 1 payload byte");
    CHECK_EQ_U32(fake_uart_tx_data()[0], 0x3C, "transmitted lead byte is '<'");
    CHECK_EQ_U32(fake_uart_tx_data()[3], MC_CMD_GET_DEVICE_TIME, "payload byte");
    CHECK_EQ_U32(fake_log_frame_count(true), 1, "outgoing frame is logged");

    meshcore_link_close(&link);
}

static void test_link_request_ok(void) {
    section("link: request returns the awaited reply");

    MeshCoreLink link;
    link_begin(&link);

    const uint8_t reply[] = {MC_RESP_OK};
    queue_reply(reply, sizeof(reply));

    uint8_t payload[64];
    size_t n = mc_cmd_set_tx_power(payload, sizeof(payload), 22);

    mc_event_t ev;
    CHECK(meshcore_link_request(&link, payload, n, MC_RESP_OK, &ev, 1000),
          "request should be answered");
    CHECK_EQ_U32(ev.code, MC_RESP_OK, "event code");
    CHECK_EQ_U32(fake_log_frame_count(false), 1, "incoming frame is logged");

    meshcore_link_close(&link);
}

static void test_link_request_skips_push(void) {
    section("link: unsolicited pushes do not satisfy a request");

    MeshCoreLink link;
    link_begin(&link);

    /* A node can advertise at any moment, including between our command and
     * its reply. That push must be stepped over, not mistaken for the answer. */
    uint8_t push[33];
    memset(push, 0, sizeof(push));
    push[0] = MC_PUSH_ADVERT;
    queue_reply(push, sizeof(push));

    const uint8_t reply[] = {MC_RESP_OK};
    queue_reply(reply, sizeof(reply));

    uint8_t payload[64];
    size_t n = mc_cmd_send_self_advert(payload, sizeof(payload), MC_ADVERT_ZERO_HOP);

    mc_event_t ev;
    CHECK(meshcore_link_request(&link, payload, n, MC_RESP_OK, &ev, 1000),
          "request should still find its reply behind the push");
    CHECK_EQ_U32(ev.code, MC_RESP_OK, "event code is the reply, not the push");
    CHECK_EQ_U32(fake_log_frame_count(false), 2, "both incoming frames are logged");

    meshcore_link_close(&link);
}

static void test_link_request_error(void) {
    section("link: a node-side error is reported, not swallowed");

    MeshCoreLink link;
    link_begin(&link);

    const uint8_t reply[] = {MC_RESP_ERR, 0x05};
    queue_reply(reply, sizeof(reply));

    uint8_t payload[64];
    size_t n = mc_cmd_app_start(payload, sizeof(payload), MESHCORE_LINK_APP_NAME);

    mc_event_t ev;
    CHECK(!meshcore_link_request(&link, payload, n, MC_RESP_SELF_INFO, &ev, 1000),
          "request must fail when the node returns ERR");
    CHECK_EQ_U32(ev.code, MC_RESP_ERR, "caller can tell ERR apart from a timeout");

    meshcore_link_close(&link);
}

static void test_link_request_timeout(void) {
    section("link: silence times out without spinning");

    MeshCoreLink link;
    link_begin(&link);
    /* Nothing queued: this is a node that is not there, or wired TX to TX. */

    uint8_t payload[64];
    size_t n = mc_cmd_app_start(payload, sizeof(payload), MESHCORE_LINK_APP_NAME);

    uint32_t started = furi_get_tick();
    mc_event_t ev;
    CHECK(!meshcore_link_request(&link, payload, n, MC_RESP_SELF_INFO, &ev, 1000),
          "request must fail on silence");
    CHECK_EQ_U32(ev.code, MESHCORE_LINK_NO_EVENT, "timeout is distinguishable from ERR");

    uint32_t elapsed = furi_get_tick() - started;
    CHECK(elapsed >= 1000, "must actually wait the timeout, waited %lu ms", (unsigned long)elapsed);
    CHECK(elapsed <= 1200, "must not overshoot the timeout, waited %lu ms", (unsigned long)elapsed);
    /* A deadline computed wrong shows up as a read storm rather than a hang. */
    CHECK(fake_uart_starved_reads() <= 3, "should block, not poll in a loop (%lu reads)",
          (unsigned long)fake_uart_starved_reads());

    meshcore_link_close(&link);
}

/* ======================================================================== *
 *  Event routing (the session's policy, without the session's threads)
 * ======================================================================== */
static void test_code_set(void) {
    section("routing: reply code sets");

    MeshCoreCodeSet one = meshcore_code_set_one(MC_RESP_OK);
    CHECK_EQ_U32(one.count, 1, "single-code set size");
    CHECK(meshcore_code_set_has(&one, MC_RESP_OK), "contains its code");
    CHECK(!meshcore_code_set_has(&one, MC_RESP_ERR), "and nothing else");

    const uint8_t sync[] = {
        MC_RESP_CONTACT_MSG_RECV,
        MC_RESP_CONTACT_MSG_RECV_V3,
        MC_RESP_CHANNEL_MSG_RECV,
        MC_RESP_CHANNEL_MSG_RECV_V3,
        MC_RESP_NO_MORE_MESSAGES,
    };
    MeshCoreCodeSet set = meshcore_code_set(sync, 5);
    CHECK_EQ_U32(set.count, 5, "SYNC_NEXT_MESSAGE reply set size");
    CHECK(set.count <= MESHCORE_CODE_SET_MAX, "and it fits the fixed set");
    for(size_t i = 0; i < 5; i++) {
        CHECK(meshcore_code_set_has(&set, sync[i]), "every SYNC reply code is accepted");
    }
    CHECK(!meshcore_code_set_has(&set, MC_RESP_SELF_INFO), "unrelated codes are not");

    /* Overflowing must clamp, not scribble past the array. */
    const uint8_t many[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    MeshCoreCodeSet clamped = meshcore_code_set(many, sizeof(many));
    CHECK_EQ_U32(clamped.count, MESHCORE_CODE_SET_MAX, "oversized input is clamped");
}

static void test_route(void) {
    section("routing: replies, streams and unsolicited pushes");

    MeshCoreCodeSet ok = meshcore_code_set_one(MC_RESP_OK);
    MeshCoreCodeSet self_info = meshcore_code_set_one(MC_RESP_SELF_INFO);
    MeshCoreCodeSet err = meshcore_code_set_one(MC_RESP_ERR);
    MeshCoreCodeSet end = meshcore_code_set_one(MC_RESP_END_OF_CONTACTS);

    /* Idle: everything is unsolicited. This is the case the messenger depends
     * on — a MSG_WAITING push arriving while the user is idle must reach the
     * application rather than being dropped. */
    CHECK(meshcore_route_event(false, &ok, false, MC_PUSH_MSG_WAITING) == MeshCoreRouteEvent,
          "idle push goes to the application");
    CHECK(meshcore_route_event(false, &ok, false, MC_RESP_OK) == MeshCoreRouteEvent,
          "even a reply-looking code is unsolicited when nothing is pending");

    /* In flight. */
    CHECK(meshcore_route_event(true, &self_info, false, MC_RESP_SELF_INFO) == MeshCoreRouteReply,
          "the awaited code completes the request");
    CHECK(meshcore_route_event(true, &self_info, false, MC_RESP_ERR) == MeshCoreRouteReply,
          "a node error ends the request");
    CHECK(meshcore_route_event(true, &err, false, MC_RESP_ERR) == MeshCoreRouteReply,
          "a request that awaits ERR still completes on ERR");

    /* A push during a plain request must not be mistaken for the answer. */
    CHECK(meshcore_route_event(true, &self_info, false, MC_PUSH_ADVERT) == MeshCoreRouteEvent,
          "a push mid-request stays unsolicited");

    /* Multi-code replies: SYNC_NEXT_MESSAGE can come back five different ways
     * and any of them ends the request. */
    const uint8_t sync[] = {
        MC_RESP_CONTACT_MSG_RECV,
        MC_RESP_CONTACT_MSG_RECV_V3,
        MC_RESP_CHANNEL_MSG_RECV,
        MC_RESP_CHANNEL_MSG_RECV_V3,
        MC_RESP_NO_MORE_MESSAGES,
    };
    MeshCoreCodeSet sync_set = meshcore_code_set(sync, 5);
    for(size_t i = 0; i < 5; i++) {
        CHECK(meshcore_route_event(true, &sync_set, false, sync[i]) == MeshCoreRouteReply,
              "each SYNC reply completes the drain step");
    }
    CHECK(meshcore_route_event(true, &sync_set, false, MC_PUSH_ADVERT) == MeshCoreRouteEvent,
          "an advert during a drain is still unsolicited");

    /* During a stream, everything non-terminating is offered to the collector,
     * which declines what is not its own — so pushes still get through. */
    CHECK(meshcore_route_event(true, &end, true, MC_RESP_CONTACT) == MeshCoreRouteStream,
          "contact records go to the collector");
    CHECK(meshcore_route_event(true, &end, true, MC_RESP_CONTACTS_START) == MeshCoreRouteStream,
          "the count header goes to the collector");
    CHECK(meshcore_route_event(true, &end, true, MC_RESP_END_OF_CONTACTS) == MeshCoreRouteReply,
          "the terminator completes the stream");
    CHECK(meshcore_route_event(true, &end, true, MC_PUSH_MSG_WAITING) == MeshCoreRouteStream,
          "a push mid-stream is offered to the collector first");
}

/* ======================================================================== *
 *  Message store
 * ======================================================================== */
static void test_messages_from_event(void) {
    section("messages: turning replies into stored messages");

    mc_event_t ev;
    MeshCoreMessage msg;

    /* Contact message, plain. */
    memset(&ev, 0, sizeof(ev));
    ev.code = MC_RESP_CONTACT_MSG_RECV;
    for(int i = 0; i < 6; i++) ev.u.contact_msg.pubkey_prefix[i] = (uint8_t)(0x10 + i);
    ev.u.contact_msg.sender_ts = 1750000000;
    ev.u.contact_msg.snr_q4 = MC_SNR_NONE;
    ev.u.contact_msg.path_len = MC_PATH_DIRECT;
    snprintf(ev.u.contact_msg.text, sizeof(ev.u.contact_msg.text), "ping from the hill");

    CHECK(meshcore_message_from_event(&ev, &msg), "a contact message converts");
    CHECK(msg.direction == MeshCoreMessageIncoming, "incoming");
    CHECK(!msg.is_channel, "not a channel message");
    CHECK_EQ_U32(msg.timestamp, 1750000000, "timestamp carried over");
    CHECK_EQ_STR(msg.text, "ping from the hill", "text carried over");
    CHECK_EQ_U32(msg.peer[0], 0x10, "peer prefix first byte");
    CHECK_EQ_U32(msg.peer[5], 0x15, "peer prefix last byte");

    /* The V3 variant differs only by carrying SNR; it must convert the same. */
    memset(&ev, 0, sizeof(ev));
    ev.code = MC_RESP_CONTACT_MSG_RECV_V3;
    ev.u.contact_msg.snr_q4 = 24; /* 6.0 dB */
    snprintf(ev.u.contact_msg.text, sizeof(ev.u.contact_msg.text), "v3");
    CHECK(meshcore_message_from_event(&ev, &msg), "the V3 variant converts too");
    CHECK_EQ_U32((uint8_t)msg.snr_q4, 24, "SNR preserved");

    /* Channel message. */
    memset(&ev, 0, sizeof(ev));
    ev.code = MC_RESP_CHANNEL_MSG_RECV;
    ev.u.channel_msg.channel_idx = 2;
    snprintf(ev.u.channel_msg.text, sizeof(ev.u.channel_msg.text), "alpha: net check");
    CHECK(meshcore_message_from_event(&ev, &msg), "a channel message converts");
    CHECK(msg.is_channel, "flagged as a channel message");
    CHECK_EQ_U32(msg.channel_idx, 2, "channel index");
    CHECK_EQ_STR(msg.text, "alpha: net check", "channel text keeps its sender prefix");

    /* Anything else is not a message. */
    memset(&ev, 0, sizeof(ev));
    ev.code = MC_RESP_NO_MORE_MESSAGES;
    CHECK(!meshcore_message_from_event(&ev, &msg), "NO_MORE_MESSAGES is not a message");
    ev.code = MC_RESP_OK;
    CHECK(!meshcore_message_from_event(&ev, &msg), "neither is OK");
}

static void add_message(MeshCoreMessages* store, const uint8_t* peer, const char* text, uint32_t ts) {
    MeshCoreMessage msg;
    memset(&msg, 0, sizeof(msg));
    memcpy(msg.peer, peer, MESHCORE_PEER_LEN);
    msg.direction = MeshCoreMessageIncoming;
    msg.timestamp = ts;
    snprintf(msg.text, sizeof(msg.text), "%s", text);
    meshcore_messages_add(store, &msg);
}

static void test_messages_ring(void) {
    section("messages: the ring drops the oldest");

    MeshCoreMessages store;
    meshcore_messages_reset(&store);

    const uint8_t peer[MESHCORE_PEER_LEN] = {1, 2, 3, 4, 5, 6};

    for(size_t i = 0; i < MESHCORE_MESSAGES_MAX; i++) {
        char text[16];
        snprintf(text, sizeof(text), "m%u", (unsigned)i);
        add_message(&store, peer, text, 1000 + (uint32_t)i);
    }
    CHECK_EQ_U32(store.count, MESHCORE_MESSAGES_MAX, "ring is full");
    CHECK_EQ_U32(store.total, MESHCORE_MESSAGES_MAX, "total counts everything added");
    CHECK_EQ_STR(meshcore_messages_at(&store, 0)->text, "m0", "oldest is first");

    /* Three more push the three oldest out. */
    add_message(&store, peer, "x0", 9000);
    add_message(&store, peer, "x1", 9001);
    add_message(&store, peer, "x2", 9002);

    CHECK_EQ_U32(store.count, MESHCORE_MESSAGES_MAX, "still full, never grows");
    CHECK_EQ_U32(store.total, MESHCORE_MESSAGES_MAX + 3, "total keeps climbing");
    CHECK_EQ_STR(meshcore_messages_at(&store, 0)->text, "m3", "the three oldest were dropped");
    CHECK_EQ_STR(
        meshcore_messages_at(&store, MESHCORE_MESSAGES_MAX - 1)->text, "x2", "newest is last");
    CHECK(meshcore_messages_at(&store, MESHCORE_MESSAGES_MAX) == NULL, "past the end is NULL");
}

static void test_messages_peer_filter(void) {
    section("messages: conversations are keyed by peer prefix");

    MeshCoreMessages store;
    meshcore_messages_reset(&store);

    const uint8_t alpha[MESHCORE_PEER_LEN] = {0xAA, 1, 2, 3, 4, 5};
    const uint8_t bravo[MESHCORE_PEER_LEN] = {0xBB, 1, 2, 3, 4, 5};

    add_message(&store, alpha, "from alpha", 1);
    add_message(&store, bravo, "from bravo", 2);
    add_message(&store, alpha, "alpha again", 3);

    /* A contact carries a full 32-byte key; only the first 6 are comparable,
     * because that is all an incoming message identifies the sender by. */
    uint8_t alpha_full[32];
    memset(alpha_full, 0x77, sizeof(alpha_full));
    memcpy(alpha_full, alpha, MESHCORE_PEER_LEN);

    CHECK_EQ_U32(meshcore_messages_count_for(&store, alpha_full), 2, "two from alpha");
    CHECK(meshcore_message_is_from(meshcore_messages_at(&store, 0), alpha_full), "first matches");
    CHECK(!meshcore_message_is_from(meshcore_messages_at(&store, 1), alpha_full),
          "bravo's does not");

    /* Channel traffic belongs to no contact conversation. */
    MeshCoreMessage channel;
    memset(&channel, 0, sizeof(channel));
    channel.is_channel = true;
    memcpy(channel.peer, alpha, MESHCORE_PEER_LEN);
    CHECK(!meshcore_message_is_from(&channel, alpha_full),
          "a channel message is not part of a direct chat");
}

/* ======================================================================== *
 *  Contact list
 * ======================================================================== */
static void make_contact(mc_contact_t* c, const char* name, uint32_t last_advert) {
    memset(c, 0, sizeof(*c));
    snprintf(c->adv_name, sizeof(c->adv_name), "%s", name);
    c->last_advert = last_advert;
    c->type = 1;
}

static void test_contacts_collect(void) {
    section("contacts: collecting a GET_CONTACTS stream");

    MeshCoreContacts contacts;
    meshcore_contacts_reset(&contacts);

    mc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.code = MC_RESP_CONTACTS_START;
    ev.u.contacts_count = 3;
    CHECK(meshcore_contacts_collect(&ev, &contacts), "CONTACTS_START belongs to the stream");
    CHECK_EQ_U32(contacts.reported, 3, "announced count is remembered");

    memset(&ev, 0, sizeof(ev));
    ev.code = MC_RESP_CONTACT;
    make_contact(&ev.u.contact, "alpha", 1000);
    CHECK(meshcore_contacts_collect(&ev, &contacts), "CONTACT belongs to the stream");
    CHECK_EQ_U32(contacts.count, 1, "contact stored");
    CHECK_EQ_STR(contacts.items[0].name, "alpha", "contact name");

    /* Anything else must be declined so it can reach the application. */
    memset(&ev, 0, sizeof(ev));
    ev.code = MC_PUSH_MSG_WAITING;
    CHECK(!meshcore_contacts_collect(&ev, &contacts), "a push is not part of the stream");
    CHECK_EQ_U32(contacts.count, 1, "and does not disturb the list");
}

static void test_contacts_overflow(void) {
    section("contacts: more peers than fit");

    MeshCoreContacts contacts;
    meshcore_contacts_reset(&contacts);

    mc_contact_t c;
    for(size_t i = 0; i < MESHCORE_CONTACTS_MAX; i++) {
        char name[16];
        snprintf(name, sizeof(name), "n%u", (unsigned)i);
        /* last_advert 1000, 1001, ... so the stalest is the first one. */
        make_contact(&c, name, 1000 + (uint32_t)i);
        CHECK(meshcore_contacts_add(&contacts, &c), "fits while there is room");
    }
    CHECK_EQ_U32(contacts.count, MESHCORE_CONTACTS_MAX, "list is full");
    CHECK_EQ_U32(contacts.dropped, 0, "nothing dropped yet");

    /* A fresher peer displaces the stalest one — a list of nodes last heard
     * from months ago is not worth the screen. */
    make_contact(&c, "fresh", 9999);
    CHECK(meshcore_contacts_add(&contacts, &c), "a fresher peer is kept");
    CHECK_EQ_U32(contacts.count, MESHCORE_CONTACTS_MAX, "still full, nothing grew");
    CHECK_EQ_U32(contacts.dropped, 1, "displacement is counted");

    bool found_fresh = false;
    bool found_stalest = false;
    for(size_t i = 0; i < contacts.count; i++) {
        if(strcmp(contacts.items[i].name, "fresh") == 0) found_fresh = true;
        if(strcmp(contacts.items[i].name, "n0") == 0) found_stalest = true;
    }
    CHECK(found_fresh, "the fresher peer is in the list");
    CHECK(!found_stalest, "the stalest peer was the one dropped");

    /* A staler peer than everything held is simply refused. */
    make_contact(&c, "ancient", 1);
    CHECK(!meshcore_contacts_add(&contacts, &c), "a staler peer is refused");
    CHECK_EQ_U32(contacts.dropped, 2, "and counted");
}

static void test_contacts_sort(void) {
    section("contacts: most recently heard first");

    MeshCoreContacts contacts;
    meshcore_contacts_reset(&contacts);

    mc_contact_t c;
    make_contact(&c, "old", 100);
    meshcore_contacts_add(&contacts, &c);
    make_contact(&c, "newest", 900);
    meshcore_contacts_add(&contacts, &c);
    make_contact(&c, "middle", 500);
    meshcore_contacts_add(&contacts, &c);

    meshcore_contacts_sort_by_last_seen(&contacts);

    CHECK_EQ_STR(contacts.items[0].name, "newest", "first entry");
    CHECK_EQ_STR(contacts.items[1].name, "middle", "second entry");
    CHECK_EQ_STR(contacts.items[2].name, "old", "third entry");
}

static void test_contacts_age(void) {
    section("contacts: rendering last seen");

    char out[MESHCORE_AGE_LEN];

    meshcore_contacts_format_age(0, 1000, out, sizeof(out));
    CHECK_EQ_STR(out, "-", "unknown node clock");

    meshcore_contacts_format_age(2000, 0, out, sizeof(out));
    CHECK_EQ_STR(out, "-", "never heard from");

    /* The node's clock can legitimately run ahead of the advert timestamp. */
    meshcore_contacts_format_age(1000, 1200, out, sizeof(out));
    CHECK_EQ_STR(out, "now", "clock skew reads as now, not as a negative age");

    meshcore_contacts_format_age(1059, 1000, out, sizeof(out));
    CHECK_EQ_STR(out, "now", "under a minute");

    meshcore_contacts_format_age(1060, 1000, out, sizeof(out));
    CHECK_EQ_STR(out, "1m", "exactly a minute");

    meshcore_contacts_format_age(1000 + 3599, 1000, out, sizeof(out));
    CHECK_EQ_STR(out, "59m", "just under an hour");

    meshcore_contacts_format_age(1000 + 7200, 1000, out, sizeof(out));
    CHECK_EQ_STR(out, "2h", "hours");

    meshcore_contacts_format_age(1000 + 86400 * 3, 1000, out, sizeof(out));
    CHECK_EQ_STR(out, "3d", "days");

    /* Whatever it renders must fit the buffer the scene gives it. */
    meshcore_contacts_format_age(0xFFFFFFFFu, 1, out, sizeof(out));
    CHECK(strlen(out) < MESHCORE_AGE_LEN, "an absurd age still fits MESHCORE_AGE_LEN");
}

/* ======================================================================== */
int main(void) {
    printf("MeshCore Config — host protocol tests\n");
    printf("library version %s\n", MESHCORE_COMPANION_VERSION);

    test_frame_lead_byte();
    test_frame_roundtrip();
    test_frame_fragmented();
    test_frame_two_in_one_read();
    test_frame_resync_after_garbage();

    test_cmd_app_start();
    test_cmd_set_radio_params();
    test_cmd_misc();

    test_parse_self_info();
    test_parse_device_info();
    test_parse_contact();

    test_link_send();
    test_link_request_ok();
    test_link_request_skips_push();
    test_link_request_error();
    test_link_request_timeout();

    test_code_set();
    test_route();

    test_messages_from_event();
    test_messages_ring();
    test_messages_peer_filter();

    test_contacts_collect();
    test_contacts_overflow();
    test_contacts_sort();
    test_contacts_age();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
