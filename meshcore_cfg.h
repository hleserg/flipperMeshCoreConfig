/*
 * MeshCore Config — Flipper Zero FAP by Greyrock Labs.
 *
 * Application-wide state, shared by every scene.
 *
 * Layering (see AGENTS.md):
 *   scenes/          UI, one file per scene
 *   uart/            furi_hal_serial wrapper            (step 2)
 *   protocol/        vendored meshcore_c + UART binding (step 3)
 *   profiles/        *.json profiles from the SD card   (step 5)
 */
#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>

#include "scenes/meshcore_scene.h"

/* View ids registered with the ViewDispatcher. One per UI module, not per
 * scene — several scenes can reuse the same module. */
typedef enum {
    MeshCoreViewSubmenu,
} MeshCoreViewId;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    /* UI modules */
    Submenu* submenu;
} MeshCoreApp;
