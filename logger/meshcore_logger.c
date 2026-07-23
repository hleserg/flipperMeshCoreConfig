#include "meshcore_logger.h"

#include <datetime/datetime.h>
#include <furi_hal_rtc.h>

#include "meshcore_csv.h"
#include "meshcore_rxlog.h"

#define MESHCORE_LOG_ROOT EXT_PATH("apps_data/meshcore_cfg")
#define MESHCORE_LOG_DIR MESHCORE_LOG_ROOT "/logs"

/* Worst case is an rx_log row: a 173-byte packet is 346 hex characters, plus
 * timestamp, signal fields and the node tags. */
#define MESHCORE_LOG_LINE_MAX 640u
/* LoRa packet rates are low, so a shallow queue is plenty; it exists to keep
 * SD latency off the UART thread, not to buffer a flood. */
#define MESHCORE_LOG_QUEUE_DEPTH 8u
#define MESHCORE_LOG_WRITER_STACK 2048u

#define MESHCORE_RX_LOG_HEADER "ts,snr,rssi,lat,lon,acc,raw,node,role,hw"

typedef struct {
    char text[MESHCORE_LOG_LINE_MAX];
} MeshCoreLogLine;

struct MeshCoreLogger {
    MeshCoreLog* log;
    Storage* storage;

    MeshCoreLogNode node;

    char session_path[96];
    MeshCoreCsv* rx_log;

    /* Rows are handed to a writer thread rather than written where they are
     * produced: a synced SD write can take tens of milliseconds, and the
     * session worker that produces them is the same thread draining the UART.
     * Stalling it there would overrun the receive buffer. */
    FuriMessageQueue* queue;
    FuriThread* writer;
    volatile bool stop;

    bool running;
    const char* error;

    volatile uint32_t rx_count;
    volatile uint32_t dropped;
    volatile bool have_rx;
    volatile int8_t last_snr_q4;
    volatile int8_t last_rssi;
};

/* ---- small formatting helpers ---- */

static const char* meshcore_logger_role_name(MeshCoreLogRole role) {
    switch(role) {
    case MeshCoreLogRoleCompanion:
        return "companion";
    case MeshCoreLogRoleRepeater:
        return "repeater";
    default:
        return "unknown";
    }
}

static const char* meshcore_logger_hw_name(MeshCoreLogHw hw) {
    switch(hw) {
    case MeshCoreLogHwT114:
        return "t114";
    case MeshCoreLogHwV4:
        return "v4";
    default:
        return "unknown";
    }
}

/* Degrees x 1e6 to a plain decimal. Done in integers so a value round-trips
 * exactly; the sign is handled separately because -0.5 has a zero whole part. */
static void meshcore_logger_format_coord(int32_t value, char* out, size_t cap) {
    const char* sign = (value < 0) ? "-" : "";
    uint32_t magnitude = (uint32_t)(value < 0 ? -(int64_t)value : (int64_t)value);
    snprintf(
        out,
        cap,
        "%s%lu.%06lu",
        sign,
        (unsigned long)(magnitude / 1000000u),
        (unsigned long)(magnitude % 1000000u));
}

/* ISO-8601 without a zone, from the Flipper RTC. Single place to change if the
 * pipeline expects something else. */
static void meshcore_logger_timestamp(char* out, size_t cap) {
    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    snprintf(
        out,
        cap,
        "%04u-%02u-%02uT%02u:%02u:%02u",
        (unsigned)now.year,
        (unsigned)now.month,
        (unsigned)now.day,
        (unsigned)now.hour,
        (unsigned)now.minute,
        (unsigned)now.second);
}

/* A node name with a comma in it would shift every later column. */
static void meshcore_logger_sanitise(const char* in, char* out, size_t cap) {
    size_t i = 0;
    for(; in[i] != '\0' && i + 1 < cap; i++) {
        char c = in[i];
        out[i] = (c == ',' || c == '"' || c == '\n' || c == '\r') ? '_' : c;
    }
    out[i] = '\0';
}

/* ---- writer thread ---- */

static int32_t meshcore_logger_writer(void* context) {
    MeshCoreLogger* logger = context;
    MeshCoreLogLine line;

    while(!logger->stop) {
        if(furi_message_queue_get(logger->queue, &line, furi_ms_to_ticks(250)) != FuriStatusOk) {
            continue;
        }
        if(logger->rx_log) meshcore_csv_write(logger->rx_log, line.text);
    }

    /* Drain whatever is still queued so a clean stop loses nothing. */
    while(furi_message_queue_get(logger->queue, &line, 0) == FuriStatusOk) {
        if(logger->rx_log) meshcore_csv_write(logger->rx_log, line.text);
    }

    return 0;
}

/* ---- event sink ---- */

/* Runs on the session worker thread. Builds the row and hands it off; no file
 * I/O here, by design. */
static void meshcore_logger_on_event(
    const mc_event_t* event,
    const uint8_t* payload,
    size_t len,
    void* context) {
    MeshCoreLogger* logger = context;

    if(event->code != MESHCORE_RXLOG_CODE) return;

    MeshCoreRxLog rx;
    if(!meshcore_rxlog_parse(payload, len, &rx)) return;

    logger->rx_count++;
    logger->last_snr_q4 = rx.snr_q4;
    logger->last_rssi = rx.rssi;
    logger->have_rx = true;

    MeshCoreLogLine line;
    /* Roomy enough for the widest year the compiler will assume. */
    char ts[32];
    char snr[MESHCORE_SNR_LEN];
    char lat[16] = "";
    char lon[16] = "";
    char name[24];

    meshcore_logger_timestamp(ts, sizeof(ts));
    meshcore_rxlog_format_snr(rx.snr_q4, snr, sizeof(snr));
    meshcore_logger_sanitise(logger->node.name, name, sizeof(name));

    if(logger->node.has_position) {
        meshcore_logger_format_coord(logger->node.lat, lat, sizeof(lat));
        meshcore_logger_format_coord(logger->node.lon, lon, sizeof(lon));
    }

    /* Hex the packet straight into the row. The buffer is sized for the
     * largest frame the firmware will send, so this does not truncate. */
    char raw[2 * 176 + 1];
    meshcore_hex_encode(rx.raw, rx.raw_len, raw, sizeof(raw));

    /* ts,snr,rssi,lat,lon,acc,raw,node,role,hw — acc is always empty, the
     * companion protocol carries no position accuracy. */
    snprintf(
        line.text,
        sizeof(line.text),
        "%s,%s,%d,%s,%s,,%s,%s,%s,%s",
        ts,
        snr,
        (int)rx.rssi,
        lat,
        lon,
        raw,
        name,
        meshcore_logger_role_name(logger->node.role),
        meshcore_logger_hw_name(logger->node.hw));

    if(furi_message_queue_put(logger->queue, &line, 0) != FuriStatusOk) {
        /* Counted rather than blocked on: losing a row beats overrunning the
         * UART and corrupting the ones that follow. */
        logger->dropped++;
    }
}

/* ---- lifecycle ---- */

MeshCoreLogger* meshcore_logger_alloc(MeshCoreLog* log) {
    MeshCoreLogger* logger = malloc(sizeof(MeshCoreLogger));

    logger->log = log;
    logger->storage = furi_record_open(RECORD_STORAGE);

    memset(&logger->node, 0, sizeof(logger->node));
    logger->node.serial_id = FuriHalSerialIdUsart;
    logger->node.role = MeshCoreLogRoleCompanion; /* stage 1: assumed */
    logger->node.hw = MeshCoreLogHwUnknown;
    snprintf(logger->node.name, sizeof(logger->node.name), "node");

    logger->session_path[0] = '\0';
    logger->rx_log = NULL;
    logger->queue = furi_message_queue_alloc(MESHCORE_LOG_QUEUE_DEPTH, sizeof(MeshCoreLogLine));
    logger->writer = NULL;
    logger->stop = false;
    logger->running = false;
    logger->error = NULL;
    logger->rx_count = 0;
    logger->dropped = 0;
    logger->have_rx = false;
    logger->last_snr_q4 = 0;
    logger->last_rssi = 0;

    return logger;
}

void meshcore_logger_free(MeshCoreLogger* logger) {
    furi_assert(logger);

    meshcore_logger_stop(logger);
    furi_message_queue_free(logger->queue);
    furi_record_close(RECORD_STORAGE);
    free(logger);
}

/* /ext/apps_data/meshcore_cfg/logs/<YYYYMMDD-HHMMSS>/ */
static bool meshcore_logger_make_session_dir(MeshCoreLogger* logger) {
    DateTime now;
    furi_hal_rtc_get_datetime(&now);

    /* Each level separately: storage_simply_mkdir does not create parents. */
    storage_simply_mkdir(logger->storage, EXT_PATH("apps_data"));
    storage_simply_mkdir(logger->storage, MESHCORE_LOG_ROOT);
    storage_simply_mkdir(logger->storage, MESHCORE_LOG_DIR);

    snprintf(
        logger->session_path,
        sizeof(logger->session_path),
        MESHCORE_LOG_DIR "/%04u%02u%02u-%02u%02u%02u",
        (unsigned)now.year,
        (unsigned)now.month,
        (unsigned)now.day,
        (unsigned)now.hour,
        (unsigned)now.minute,
        (unsigned)now.second);

    return storage_simply_mkdir(logger->storage, logger->session_path);
}

/* Ask the node who it is, so rows can be tagged and positions filled in. */
static void meshcore_logger_read_node(MeshCoreLogger* logger) {
    uint8_t payload[MC_MAX_PAYLOAD];
    mc_event_t event;

    size_t len = mc_cmd_app_start(payload, sizeof(payload), MESHCORE_LINK_APP_NAME);
    if(len == 0) return;

    if(!meshcore_session_request(
           logger->node.session, payload, len, MC_RESP_SELF_INFO, &event, MESHCORE_LINK_TIMEOUT_MS)) {
        /* Not fatal: RX log pushes do not depend on the handshake, so we can
         * still record signal data from an unidentified node. */
        meshcore_log_printf(logger->log, "logger: no self-info, tagging as unknown");
        return;
    }

    if(event.u.self_info.name[0] != '\0') {
        /* A node name can be up to MC_MAX_TEXT; the tag column wants
         * something short, and a truncated name still identifies the node. */
        snprintf(logger->node.name, sizeof(logger->node.name), "%.23s", event.u.self_info.name);
    }

    /* A node that has never been given a position advertises 0,0. Treating
     * that as a real fix would put every walk off the coast of Africa. */
    if(event.u.self_info.adv_lat != 0 || event.u.self_info.adv_lon != 0) {
        logger->node.lat = event.u.self_info.adv_lat;
        logger->node.lon = event.u.self_info.adv_lon;
        logger->node.has_position = true;
    }

    /* Hardware detection proper is stage 4; DEVICE_INFO carries the model. */
    len = mc_cmd_device_query(payload, sizeof(payload), MESHCORE_LINK_PROTO_VER);
    if(len != 0 && meshcore_session_request(
                       logger->node.session,
                       payload,
                       len,
                       MC_RESP_DEVICE_INFO,
                       &event,
                       MESHCORE_LINK_TIMEOUT_MS)) {
        const char* model = event.u.device_info.model;
        if(strstr(model, "T114") || strstr(model, "t114")) {
            logger->node.hw = MeshCoreLogHwT114;
        } else if(strstr(model, "V4") || strstr(model, "v4")) {
            logger->node.hw = MeshCoreLogHwV4;
        }
    }
}

bool meshcore_logger_start(MeshCoreLogger* logger) {
    furi_assert(logger);
    if(logger->running) return true;

    logger->error = NULL;
    logger->rx_count = 0;
    logger->dropped = 0;
    logger->have_rx = false;

    if(!meshcore_logger_make_session_dir(logger)) {
        logger->error = "Cannot create the log folder.\nIs an SD card inserted?";
        return false;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/rx_log.csv", logger->session_path);
    logger->rx_log = meshcore_csv_open(logger->storage, path, MESHCORE_RX_LOG_HEADER);
    if(!logger->rx_log) {
        logger->error = "Cannot open rx_log.csv.";
        return false;
    }

    logger->node.session = meshcore_session_alloc(logger->log, logger->node.serial_id);
    meshcore_session_set_event_callback(logger->node.session, meshcore_logger_on_event, logger);

    if(!meshcore_session_start(logger->node.session)) {
        meshcore_session_free(logger->node.session);
        logger->node.session = NULL;
        meshcore_csv_close(logger->rx_log);
        logger->rx_log = NULL;
        logger->error = "Cannot take the UART.\nAnother app is holding it.";
        return false;
    }

    logger->stop = false;
    logger->writer = furi_thread_alloc_ex(
        "MeshCoreLogWriter", MESHCORE_LOG_WRITER_STACK, meshcore_logger_writer, logger);
    furi_thread_start(logger->writer);

    logger->running = true;

    /* After the writer is up, so anything the handshake shakes loose is
     * recorded rather than dropped. */
    meshcore_logger_read_node(logger);

    meshcore_log_printf(logger->log, "logger: %s", logger->session_path);
    return true;
}

void meshcore_logger_stop(MeshCoreLogger* logger) {
    furi_assert(logger);
    if(!logger->running) return;

    /* Session first: it feeds the queue, so it has to stop producing before
     * the writer stops consuming. */
    if(logger->node.session) {
        meshcore_session_free(logger->node.session);
        logger->node.session = NULL;
    }

    logger->stop = true;
    furi_thread_join(logger->writer);
    furi_thread_free(logger->writer);
    logger->writer = NULL;

    meshcore_csv_close(logger->rx_log);
    logger->rx_log = NULL;

    logger->running = false;
}

bool meshcore_logger_is_running(MeshCoreLogger* logger) {
    furi_assert(logger);
    return logger->running;
}

const char* meshcore_logger_error(MeshCoreLogger* logger) {
    furi_assert(logger);
    return logger->error;
}

const char* meshcore_logger_session_path(MeshCoreLogger* logger) {
    furi_assert(logger);
    return logger->session_path;
}

const MeshCoreLogNode* meshcore_logger_node(MeshCoreLogger* logger) {
    furi_assert(logger);
    return &logger->node;
}

uint32_t meshcore_logger_rx_count(MeshCoreLogger* logger) {
    furi_assert(logger);
    return logger->rx_count;
}

uint32_t meshcore_logger_dropped(MeshCoreLogger* logger) {
    furi_assert(logger);
    return logger->dropped;
}

bool meshcore_logger_last_rx(MeshCoreLogger* logger, int8_t* snr_q4, int8_t* rssi) {
    furi_assert(logger);
    if(!logger->have_rx) return false;
    if(snr_q4) *snr_q4 = logger->last_snr_q4;
    if(rssi) *rssi = logger->last_rssi;
    return true;
}
