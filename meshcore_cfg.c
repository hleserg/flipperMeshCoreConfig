#include "meshcore_cfg.h"

#include "detect/meshcore_detect.h"

static bool meshcore_cfg_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    MeshCoreApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

/* Returning false here pops the last scene and stops the ViewDispatcher,
 * which is how Back on the root scene exits the app. */
static bool meshcore_cfg_back_event_callback(void* context) {
    furi_assert(context);
    MeshCoreApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void meshcore_cfg_tick_event_callback(void* context) {
    furi_assert(context);
    MeshCoreApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

/* Runs on the session worker thread whenever the node sends something nobody
 * asked for. Must stay trivial: no blocking, no GUI. Stage 2 replaces this
 * with real handling of MSG_WAITING. */
static void meshcore_cfg_push_callback(
    const mc_event_t* event,
    const uint8_t* payload,
    size_t len,
    void* context) {
    MeshCoreApp* app = context;
    UNUSED(payload);
    UNUSED(len);
    app->push_count++;
    app->last_push_code = event->code;

    /* "Drain me": the node has mail for us. Waking the mailbox only sets an
     * event flag, which is all this callback may do — it runs on the session
     * worker, and draining blocks on that same worker's replies. */
    if(event->code == MC_PUSH_MSG_WAITING) {
        meshcore_mailbox_notify(app->mailbox);
    }
}

static MeshCoreApp* meshcore_cfg_app_alloc(void) {
    MeshCoreApp* app = malloc(sizeof(MeshCoreApp));

    /* Tell any node wired to us that the Flipper is here, before anything
     * else: a node sampling its detect input while we were still booting
     * would come up on radio instead of serial. */
    meshcore_detect_init();

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&meshcore_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, meshcore_cfg_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, meshcore_cfg_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, meshcore_cfg_tick_event_callback, MESHCORE_TICK_PERIOD_MS);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, MeshCoreViewSubmenu, submenu_get_view(app->submenu));

    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, MeshCoreViewWidget, widget_get_view(app->widget));

    app->text_box = text_box_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, MeshCoreViewTextBox, text_box_get_view(app->text_box));

    app->loading = loading_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, MeshCoreViewLoading, loading_get_view(app->loading));

    app->log = meshcore_log_alloc();
    /* Configurator and Messenger share one node on the USART. The Logger runs
     * its own session, so it can sit on either port. */
    app->session = meshcore_session_alloc(app->log, FuriHalSerialIdUsart);
    app->logger = meshcore_logger_alloc(app->log);
    memset(&app->node, 0, sizeof(app->node));

    meshcore_contacts_reset(&app->contacts);
    meshcore_messages_reset(&app->messages);
    /* Built-ins only for now; the card is read the first time Profiles opens,
     * so a session that never goes there never waits on storage. */
    meshcore_preset_store_init(&app->presets);
    app->preset_index = 0;
    memset(app->apply_result, 0, sizeof(app->apply_result));
    memset(app->chat_peer, 0, sizeof(app->chat_peer));
    app->chat_peer_name[0] = '\0';
    app->node_time = 0;
    app->push_count = 0;
    app->last_push_code = 0;

    /* The mailbox must exist before the push callback can wake it. Its worker
     * runs for the app's lifetime and no-ops until the session is connected,
     * so mail queued on the node is picked up as soon as we attach. */
    app->mailbox = meshcore_mailbox_alloc(app->session, &app->messages);
    meshcore_session_set_event_callback(app->session, meshcore_cfg_push_callback, app);
    meshcore_mailbox_start(app->mailbox);

    app->worker = NULL;
    app->worker_stop = false;
    app->worker_error = NULL;

    app->text_buf[0] = furi_string_alloc();
    app->text_buf[1] = furi_string_alloc();
    app->text_buf_slot = 0;

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    return app;
}

static void meshcore_cfg_app_free(MeshCoreApp* app) {
    furi_assert(app);

    /* Scenes join their own workers on exit, so nothing should be running by
     * the time we get here. Freeing the session stops its worker and releases
     * the USART. */
    /* Mailbox first: its worker calls into the session, so it must be gone
     * before the session is. */
    meshcore_mailbox_free(app->mailbox);
    meshcore_logger_free(app->logger);
    meshcore_session_free(app->session);
    /* After everything that logs has stopped, so the file has the whole run in
     * it, and before the log itself goes away. */
    meshcore_log_dump(app->log);
    meshcore_log_free(app->log);

    view_dispatcher_remove_view(app->view_dispatcher, MeshCoreViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, MeshCoreViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, MeshCoreViewTextBox);
    view_dispatcher_remove_view(app->view_dispatcher, MeshCoreViewLoading);

    submenu_free(app->submenu);
    widget_free(app->widget);
    text_box_free(app->text_box);
    loading_free(app->loading);

    furi_string_free(app->text_buf[0]);
    furi_string_free(app->text_buf[1]);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    /* Release the detect lines last, so a node rebooted after we exit sees
     * LOW and brings its own radio up. */
    meshcore_detect_deinit();
    free(app);
}

/* Launch argument -> the screen to open on. The menu is always underneath, so
 * Back still goes where the user expects.
 *
 * This exists to make the app testable without hands: the Flipper's `input
 * send` does not reach a running application, so a scene that can only be
 * opened by pressing OK can only be exercised by pressing OK. With this,
 *
 *     loader open "MeshCore Config" logger
 *
 * starts a logging session over the CLI, which is how a change to the logger
 * gets checked against a real node before anyone walks anywhere with it. */
static const struct {
    const char* name;
    MeshCoreSceneId scene;
} meshcore_cfg_launch_args[] = {
    {"logger", MeshCoreSceneLogger},
    {"connect", MeshCoreSceneConnect},
    {"contacts", MeshCoreSceneContacts},
    {"profiles", MeshCoreSceneProfiles},
    {"log", MeshCoreSceneLog},
};

int32_t meshcore_cfg_app(void* p) {
    const char* arg = p;

    MeshCoreApp* app = meshcore_cfg_app_alloc();
    app->launch_scene = MeshCoreSceneNum;

    if(arg != NULL && arg[0] != '\0') {
        for(size_t i = 0; i < COUNT_OF(meshcore_cfg_launch_args); i++) {
            if(strcmp(arg, meshcore_cfg_launch_args[i].name) == 0) {
                app->launch_scene = meshcore_cfg_launch_args[i].scene;
                break;
            }
        }
        /* An unknown argument leaves the menu open rather than failing to
         * start: a typo should not look like a crash. */
    }

    /* The menu opens the requested scene on its first tick, once the dispatcher
     * is serving its queue. Doing it here instead crashes: a scene whose
     * on_enter starts a worker would have that worker post an event into a
     * queue nobody is reading yet. */
    scene_manager_next_scene(app->scene_manager, MeshCoreSceneMenu);

    view_dispatcher_run(app->view_dispatcher);
    meshcore_cfg_app_free(app);

    return 0;
}
