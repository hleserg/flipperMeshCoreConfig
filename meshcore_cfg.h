/*
 * MeshCore Config — Flipper Zero FAP by Greyrock Labs.
 *
 * Application-wide state, shared by every scene.
 *
 * Layering (see AGENTS.md):
 *   uart/            furi_hal_serial wrapper — moves bytes, knows no protocol
 *   protocol/        vendored meshcore_c + its binding to the UART layer
 *   scenes/          UI, one file per scene
 *   profiles/        *.json profiles from the SD card       (step 5)
 */
#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/widget.h>

#include "meshcore_log.h"
#include "protocol/meshcore_link.h"
#include "scenes/meshcore_scene.h"

/* How often scenes get a tick event (used by the log view to refresh). */
#define MESHCORE_TICK_PERIOD_MS 500u

/* View ids registered with the ViewDispatcher. One per UI module, not per
 * scene — several scenes reuse the same module. */
typedef enum {
    MeshCoreViewSubmenu,
    MeshCoreViewWidget,
    MeshCoreViewTextBox,
} MeshCoreViewId;

/* What the node told us about itself. Filled by scene_connect from SELF_INFO
 * and DEVICE_INFO; the editors in later steps read and modify it. */
typedef struct {
    bool valid;

    /* DEVICE_INFO */
    char model[MC_MAX_MODEL];
    char fw_ver[24];
    int8_t fw_ver_code;

    /* SELF_INFO */
    char name[MC_MAX_TEXT];
    uint32_t freq_khz; /* radio_freq, kHz — 869525 == 869.525 MHz */
    uint32_t bw_khz;
    uint8_t sf;
    uint8_t cr;
    uint8_t tx_power; /* dBm */
    uint8_t max_tx_power; /* dBm */
    uint8_t role_type; /* advert type byte; see the Role caveat in AGENTS.md */
} MeshCoreNodeInfo;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    /* UI modules */
    Submenu* submenu;
    Widget* widget;
    TextBox* text_box;

    /* Transport + protocol */
    MeshCoreLog* log;
    MeshCoreLink link;
    MeshCoreNodeInfo node;

    /* Worker thread owned by whichever scene is currently running one. */
    FuriThread* worker;
    volatile bool worker_stop;
    /* Set by the worker, read by the GUI thread after the completion event.
     * Always a string literal, so there is no ownership to worry about. */
    const char* worker_error;

    /* TextBox stores the pointer it is given rather than copying, and the GUI
     * thread may be drawing from it at any moment. Two buffers let the log
     * view build the next snapshot without touching the one on screen. */
    FuriString* text_buf[2];
    uint8_t text_buf_slot;
} MeshCoreApp;
