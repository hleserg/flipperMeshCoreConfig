#include "../meshcore_cfg.h"

/* Item order == submenu order == custom event id. */
typedef enum {
    MeshCoreMenuIndexConnect,
    MeshCoreMenuIndexRadio,
    MeshCoreMenuIndexIdentity,
    MeshCoreMenuIndexRole,
    MeshCoreMenuIndexProfiles,
    MeshCoreMenuIndexSendAdvert,
    MeshCoreMenuIndexSerialLog,
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

    ITEM(app->node.valid ? "Reconnect" : "Connect", MeshCoreMenuIndexConnect);
    ITEM("Radio", MeshCoreMenuIndexRadio);
    ITEM("Identity", MeshCoreMenuIndexIdentity);
    ITEM("Role", MeshCoreMenuIndexRole);
    ITEM("Profiles", MeshCoreMenuIndexProfiles);
    ITEM("Send advert", MeshCoreMenuIndexSendAdvert);
    ITEM("Serial log", MeshCoreMenuIndexSerialLog);

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

        switch(event.event) {
        case MeshCoreMenuIndexConnect:
            scene_manager_next_scene(app->scene_manager, MeshCoreSceneConnect);
            break;
        case MeshCoreMenuIndexSerialLog:
            scene_manager_next_scene(app->scene_manager, MeshCoreSceneLog);
            break;
        default:
            /* Radio / Identity / Role / Profiles / Send advert land in steps
             * 4 and 5; the selection is remembered until then. */
            break;
        }

        return true;
    }

    return false;
}

void meshcore_scene_menu_on_exit(void* context) {
    MeshCoreApp* app = context;
    submenu_reset(app->submenu);
}
