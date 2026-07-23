#include "meshcore_cfg.h"

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
    free(app);
}

int32_t meshcore_cfg_app(void* p) {
    UNUSED(p);

    MeshCoreApp* app = meshcore_cfg_app_alloc();
    scene_manager_next_scene(app->scene_manager, MeshCoreSceneMenu);
    view_dispatcher_run(app->view_dispatcher);
    meshcore_cfg_app_free(app);

    return 0;
}
