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

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, MeshCoreViewSubmenu, submenu_get_view(app->submenu));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    return app;
}

static void meshcore_cfg_app_free(MeshCoreApp* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, MeshCoreViewSubmenu);
    submenu_free(app->submenu);

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
