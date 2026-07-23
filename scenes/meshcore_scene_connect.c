/*
 * scene_connect — open the UART, handshake with the node and show what it is.
 *
 * The protocol calls block, so they run on a worker thread; the GUI thread is
 * only woken by a custom event when the worker is done. Nothing below the
 * scene layer ever touches a view.
 */
#include "../meshcore_cfg.h"

#define MESHCORE_CONNECT_EVENT_DONE 1u
#define MESHCORE_CONNECT_WORKER_STACK 2048u

static void meshcore_connect_copy_self_info(MeshCoreNodeInfo* node, const mc_self_info_t* info) {
    node->role_type = info->type;
    node->tx_power = info->tx_power;
    node->max_tx_power = info->max_tx_power;
    node->freq_khz = info->radio_freq;
    node->bw_khz = info->radio_bw;
    node->sf = info->radio_sf;
    node->cr = info->radio_cr;
    snprintf(node->name, sizeof(node->name), "%s", info->name);
}

/* Turn a failed request into something worth reading on a 128x64 screen. */
static const char* meshcore_connect_diagnose(MeshCoreApp* app, const mc_event_t* ev) {
    if(ev->code == MC_RESP_ERR) return "Node refused the handshake.";
    if(meshcore_link_rx_errors(&app->link) > 0) return "Noise on the line.\nWrong baud, or TX/RX swapped?";
    return "No reply from the node.\nIs it on a hardware UART?";
}

/* Runs on the worker thread. Returns NULL on success, or a literal to show. */
static const char* meshcore_connect_run(MeshCoreApp* app) {
    MeshCoreNodeInfo* node = &app->node;
    uint8_t payload[MC_MAX_PAYLOAD];
    mc_event_t ev;
    size_t len;

    node->valid = false;

    if(!meshcore_link_is_open(&app->link)) {
        if(!meshcore_link_open(&app->link, app->log)) {
            return "Cannot take the USART.\nAnother app is holding it.";
        }
    }

    /* Whatever the node said before we showed up is not a reply to us. */
    meshcore_link_flush(&app->link);

    /* APP_START identifies us and comes back with the node's current radio
     * settings, which is also the config the editors will work on. */
    len = mc_cmd_app_start(payload, sizeof(payload), MESHCORE_LINK_APP_NAME);
    if(len == 0) return "Internal error: APP_START.";
    if(!meshcore_link_request(
           &app->link, payload, len, MC_RESP_SELF_INFO, &ev, MESHCORE_LINK_TIMEOUT_MS)) {
        return meshcore_connect_diagnose(app, &ev);
    }
    meshcore_connect_copy_self_info(node, &ev.u.self_info);

    if(app->worker_stop) return "Cancelled.";

    /* DEVICE_QUERY gives model and firmware version. Older firmware may not
     * answer it — that is not fatal, we already have the settings. */
    len = mc_cmd_device_query(payload, sizeof(payload), MESHCORE_LINK_PROTO_VER);
    if(len != 0 && meshcore_link_request(
                       &app->link, payload, len, MC_RESP_DEVICE_INFO, &ev,
                       MESHCORE_LINK_TIMEOUT_MS)) {
        snprintf(node->model, sizeof(node->model), "%s", ev.u.device_info.model);
        snprintf(node->fw_ver, sizeof(node->fw_ver), "%s", ev.u.device_info.ver);
        node->fw_ver_code = ev.u.device_info.fw_ver;
    } else {
        snprintf(node->model, sizeof(node->model), "MeshCore node");
        snprintf(node->fw_ver, sizeof(node->fw_ver), "?");
        node->fw_ver_code = 0;
        meshcore_log_printf(app->log, "no DEVICE_INFO, using SELF_INFO only");
    }

    node->valid = true;
    return NULL;
}

static int32_t meshcore_connect_worker(void* context) {
    MeshCoreApp* app = context;
    app->worker_error = meshcore_connect_run(app);
    view_dispatcher_send_custom_event(app->view_dispatcher, MESHCORE_CONNECT_EVENT_DONE);
    return 0;
}

static void meshcore_scene_connect_show(MeshCoreApp* app, bool done) {
    /* Widget elements copy the text they are given, so a stack buffer is fine.
     * Every %s below is length-capped: the node name alone can be 184 bytes,
     * which neither the screen nor this buffer has any use for. */
    char text[512];

    widget_reset(app->widget);

    if(!done) {
        snprintf(
            text,
            sizeof(text),
            "\e#Connecting...\n\n"
            "13 = TX  ->  node RX\n"
            "14 = RX  <-  node TX\n"
            "18 = GND\n"
            "115200 8N1");
    } else if(app->worker_error) {
        snprintf(
            text,
            sizeof(text),
            "\e#Not connected\n"
            "%s\n\n"
            "13=TX 14=RX 18=GND, 115200\n"
            "line errors: %lu",
            app->worker_error,
            (unsigned long)meshcore_link_rx_errors(&app->link));
    } else {
        const MeshCoreNodeInfo* n = &app->node;
        snprintf(
            text,
            sizeof(text),
            "\e#%.32s\n"
            "fw %.20s (v%d)\n"
            "name: %.48s\n"
            "freq: %lu.%03lu MHz\n"
            "bw %lu kHz  sf %u  cr %u\n"
            "tx %u dBm (max %u)",
            n->model,
            n->fw_ver,
            (int)n->fw_ver_code,
            n->name,
            (unsigned long)(n->freq_khz / 1000u),
            (unsigned long)(n->freq_khz % 1000u),
            (unsigned long)n->bw_khz,
            (unsigned)n->sf,
            (unsigned)n->cr,
            (unsigned)n->tx_power,
            (unsigned)n->max_tx_power);
    }

    widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, text);
}

void meshcore_scene_connect_on_enter(void* context) {
    MeshCoreApp* app = context;

    app->worker_stop = false;
    app->worker_error = NULL;

    meshcore_scene_connect_show(app, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, MeshCoreViewWidget);

    app->worker = furi_thread_alloc_ex(
        "MeshCoreConnect", MESHCORE_CONNECT_WORKER_STACK, meshcore_connect_worker, app);
    furi_thread_start(app->worker);
}

bool meshcore_scene_connect_on_event(void* context, SceneManagerEvent event) {
    MeshCoreApp* app = context;

    if(event.type == SceneManagerEventTypeCustom && event.event == MESHCORE_CONNECT_EVENT_DONE) {
        meshcore_scene_connect_show(app, true);
        return true;
    }

    return false;
}

void meshcore_scene_connect_on_exit(void* context) {
    MeshCoreApp* app = context;

    if(app->worker) {
        /* Requests time out in MESHCORE_LINK_TIMEOUT_MS, so the join is
         * bounded by roughly that even when the node is silent. */
        app->worker_stop = true;
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }

    widget_reset(app->widget);
}
