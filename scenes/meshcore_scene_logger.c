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

/* The static screens can scroll: nothing redraws them, so the scroll position
 * survives. */
static void meshcore_scene_logger_show_text(MeshCoreApp* app, const char* text) {
    widget_reset(app->widget);
    widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, text);
}

/* The live screen must NOT be a scrolling element. It is rebuilt whenever a
 * packet lands, and rebuilding a text scroll element resets its scroll
 * position — which reads as "the screen refuses to scroll". So this lays out
 * fixed lines that fit 128x64 with nothing to scroll to. */
static void meshcore_scene_logger_show_running(MeshCoreApp* app) {
    const MeshCoreLogNode* node = meshcore_logger_node(app->logger);

    char line[48];
    widget_reset(app->widget);

    widget_add_string_element(
        app->widget, 0, 10, AlignLeft, AlignBottom, FontPrimary, "Logging");

    snprintf(line, sizeof(line), "%.20s", node->name);
    widget_add_string_element(app->widget, 64, 10, AlignLeft, AlignBottom, FontSecondary, line);

    snprintf(line, sizeof(line), "packets: %lu", (unsigned long)meshcore_logger_rx_count(app->logger));
    widget_add_string_element(app->widget, 0, 24, AlignLeft, AlignBottom, FontSecondary, line);

    int8_t snr_q4 = 0;
    int8_t rssi = 0;
    if(meshcore_logger_last_rx(app->logger, &snr_q4, &rssi)) {
        char snr[MESHCORE_SNR_LEN];
        meshcore_rxlog_format_snr(snr_q4, snr, sizeof(snr));
        snprintf(line, sizeof(line), "SNR %s  RSSI %d", snr, (int)rssi);
    } else {
        snprintf(line, sizeof(line), "waiting for traffic");
    }
    widget_add_string_element(app->widget, 0, 36, AlignLeft, AlignBottom, FontSecondary, line);

    /* Just the session name; the full path would not fit and the user already
     * knows the directory it lives in. */
    const char* path = meshcore_logger_session_path(app->logger);
    const char* leaf = strrchr(path, '/');
    snprintf(line, sizeof(line), "%s", leaf ? leaf + 1 : path);
    widget_add_string_element(app->widget, 0, 48, AlignLeft, AlignBottom, FontSecondary, line);

    uint32_t dropped = meshcore_logger_dropped(app->logger);
    if(dropped > 0) {
        /* Only shown when it happens — a dropped row means the card could not
         * keep up, which the user needs to know about while still recording. */
        snprintf(line, sizeof(line), "dropped: %lu", (unsigned long)dropped);
    } else {
        snprintf(line, sizeof(line), "Back = stop");
    }
    widget_add_string_element(app->widget, 0, 62, AlignLeft, AlignBottom, FontSecondary, line);
}

static void meshcore_scene_logger_show(MeshCoreApp* app, bool started) {
    if(!started) {
        meshcore_scene_logger_show_text(
            app,
            "\e#Logger\n"
            "Starting...\n\n"
            "13 = TX  ->  node RX\n"
            "14 = RX  <-  node TX\n"
            "18 = GND");
        return;
    }

    const char* error = meshcore_logger_error(app->logger);
    if(error) {
        char text[256];
        snprintf(text, sizeof(text), "\e#Logger failed\n%s", error);
        meshcore_scene_logger_show_text(app, text);
        return;
    }

    meshcore_scene_logger_show_running(app);
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
        scene_manager_set_scene_state(
            app->scene_manager, MeshCoreSceneLogger, meshcore_logger_rx_count(app->logger) + 1);
        return true;
    }

    if(event.type == SceneManagerEventTypeTick) {
        /* Only once the worker is done, or the display would race the start. */
        if(!app->worker && meshcore_logger_is_running(app->logger)) {
            /* Redraw only when a packet actually landed. Rebuilding on every
             * tick regardless would make the screen flicker and would fight
             * anything the user is doing with it. The +1 keeps 0 meaning
             * "never drawn". Every counter on screen moves with rx_count, so
             * it is a sufficient signature. */
            uint32_t shown = scene_manager_get_scene_state(app->scene_manager, MeshCoreSceneLogger);
            uint32_t now = meshcore_logger_rx_count(app->logger) + 1;
            if(shown != now) {
                scene_manager_set_scene_state(app->scene_manager, MeshCoreSceneLogger, now);
                meshcore_scene_logger_show(app, true);
            }
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
