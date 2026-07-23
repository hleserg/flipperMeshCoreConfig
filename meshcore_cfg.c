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

    app->log = meshcore_log_alloc();
    meshcore_link_init(&app->link);
    memset(&app->node, 0, sizeof(app->node));

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
     * the time we get here — but the USART may still be held. */
    meshcore_link_close(&app->link);
    meshcore_log_free(app->log);

    view_dispatcher_remove_view(app->view_dispatcher, MeshCoreViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, MeshCoreViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, MeshCoreViewTextBox);

    submenu_free(app->submenu);
    widget_free(app->widget);
    text_box_free(app->text_box);

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
