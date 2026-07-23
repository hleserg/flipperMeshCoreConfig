/*
 * scene_logger — field logging: attach to the node, record what it hears.
 *
 * Back stops the session and closes the files. That is deliberate: holding the
 * UART while the user wanders off into other screens would be a surprising way
 * to lose a walk's worth of data.
 */
#include "../logger/meshcore_rxlog.h"
#include "../meshcore_cfg.h"

#define MESHCORE_LOGGER_EVENT_STARTED 0x300u
#define MESHCORE_LOGGER_WORKER_STACK 2048u

static int32_t meshcore_logger_scene_worker(void* context) {
    MeshCoreApp* app = context;
    meshcore_logger_start(app->logger);
    view_dispatcher_send_custom_event(app->view_dispatcher, MESHCORE_LOGGER_EVENT_STARTED);
    return 0;
}

static void meshcore_scene_logger_show(MeshCoreApp* app, bool started) {
    char text[512];

    if(!started) {
        snprintf(
            text,
            sizeof(text),
            "\e#Logger\n"
            "Starting...\n\n"
            "13 = TX  ->  node RX\n"
            "14 = RX  <-  node TX\n"
            "18 = GND");
    } else {
        const char* error = meshcore_logger_error(app->logger);
        if(error) {
            snprintf(text, sizeof(text), "\e#Logger failed\n%s", error);
        } else {
            const MeshCoreLogNode* node = meshcore_logger_node(app->logger);

            char signal[32] = "waiting for traffic";
            int8_t snr_q4 = 0;
            int8_t rssi = 0;
            if(meshcore_logger_last_rx(app->logger, &snr_q4, &rssi)) {
                char snr[MESHCORE_SNR_LEN];
                meshcore_rxlog_format_snr(snr_q4, snr, sizeof(snr));
                snprintf(signal, sizeof(signal), "SNR %s dB  RSSI %d", snr, (int)rssi);
            }

            char dropped[32] = "";
            if(meshcore_logger_dropped(app->logger) > 0) {
                /* Only shown when it happens — a dropped row means the card
                 * could not keep up, which the user needs to know about. */
                snprintf(
                    dropped,
                    sizeof(dropped),
                    "\ndropped: %lu",
                    (unsigned long)meshcore_logger_dropped(app->logger));
            }

            snprintf(
                text,
                sizeof(text),
                "\e#Logging\n"
                "%.20s\n"
                "packets: %lu\n"
                "%s%s\n\n"
                "%s\n\n"
                "Back stops and closes files.",
                node->name,
                (unsigned long)meshcore_logger_rx_count(app->logger),
                signal,
                dropped,
                meshcore_logger_session_path(app->logger));
        }
    }

    widget_reset(app->widget);
    widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, text);
}

void meshcore_scene_logger_on_enter(void* context) {
    MeshCoreApp* app = context;

    meshcore_scene_logger_show(app, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, MeshCoreViewWidget);

    app->worker = furi_thread_alloc_ex(
        "MeshCoreLoggerUi", MESHCORE_LOGGER_WORKER_STACK, meshcore_logger_scene_worker, app);
    furi_thread_start(app->worker);
}

bool meshcore_scene_logger_on_event(void* context, SceneManagerEvent event) {
    MeshCoreApp* app = context;

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == MESHCORE_LOGGER_EVENT_STARTED) {
        /* The worker posted this as its last act, so joining is immediate.
         * Clearing it here is what lets the tick handler take over refreshing. */
        if(app->worker) {
            furi_thread_join(app->worker);
            furi_thread_free(app->worker);
            app->worker = NULL;
        }
        meshcore_scene_logger_show(app, true);
        return true;
    }

    if(event.type == SceneManagerEventTypeTick) {
        /* Only once the worker is done, or the display would race the start. */
        if(!app->worker && meshcore_logger_is_running(app->logger)) {
            meshcore_scene_logger_show(app, true);
        }
        return true;
    }

    return false;
}

void meshcore_scene_logger_on_exit(void* context) {
    MeshCoreApp* app = context;

    if(app->worker) {
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }

    meshcore_logger_stop(app->logger);
    widget_reset(app->widget);
}
