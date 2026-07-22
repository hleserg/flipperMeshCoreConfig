#include "../meshcore_cfg.h"

/* Item order == submenu order == custom event id. */
typedef enum {
    MeshCoreMenuIndexConnect,
    MeshCoreMenuIndexRadio,
    MeshCoreMenuIndexIdentity,
    MeshCoreMenuIndexRole,
    MeshCoreMenuIndexProfiles,
    MeshCoreMenuIndexSendAdvert,
} MeshCoreMenuIndex;

static void meshcore_scene_menu_submenu_callback(void* context, uint32_t index) {
    MeshCoreApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void meshcore_scene_menu_on_enter(void* context) {
    MeshCoreApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_reset(submenu);
    submenu_set_header(submenu, "MeshCore Config");

#define ITEM(label, idx) \
    submenu_add_item(submenu, label, idx, meshcore_scene_menu_submenu_callback, app)

    ITEM("Connect", MeshCoreMenuIndexConnect);
    ITEM("Radio", MeshCoreMenuIndexRadio);
    ITEM("Identity", MeshCoreMenuIndexIdentity);
    ITEM("Role", MeshCoreMenuIndexRole);
    ITEM("Profiles", MeshCoreMenuIndexProfiles);
    ITEM("Send advert", MeshCoreMenuIndexSendAdvert);

#undef ITEM

    /* Restore the cursor where the user left it. */
    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, MeshCoreSceneMenu));

    view_dispatcher_switch_to_view(app->view_dispatcher, MeshCoreViewSubmenu);
}

bool meshcore_scene_menu_on_event(void* context, SceneManagerEvent event) {
    MeshCoreApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, MeshCoreSceneMenu, event.event);
        /* Step 1 is a static list: the target scenes do not exist yet, so the
         * selection is only remembered. Navigation is wired up in steps 2-5. */
        return true;
    }

    return false;
}

void meshcore_scene_menu_on_exit(void* context) {
    MeshCoreApp* app = context;
    submenu_reset(app->submenu);
}
