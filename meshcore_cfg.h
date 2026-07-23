/*
 * MeshCore Config — Flipper Zero FAP by Greyrock Labs.
 *
 * Two modes over one link to the node:
 *   Configurator — read and change the node's radio settings and identity
 *   Messenger    — use the node as a radio: contacts, chats, adverts
 *
 * The radio is never on the Flipper. The node does the meshing and holds the
 * keys; this app is a UI for it, speaking the MeshCore companion protocol over
 * the hardware UART.
 *
 * Layering (see AGENTS.md):
 *   uart/            furi_hal_serial wrapper — moves bytes, knows no protocol
 *   protocol/        vendored meshcore_c, its UART binding, and the session
 *                    worker that owns the link for as long as we are connected
 *   messenger/       contact list and, later, message history
 *   scenes/          UI, one file per scene
 *   profiles/        *.json profiles from the SD card       (configurator, later)
 */
#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/loading.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/widget.h>

#include "config/meshcore_apply.h"
#include "config/meshcore_preset_store.h"
#include "logger/meshcore_logger.h"
#include "meshcore_log.h"
#include "messenger/meshcore_contacts.h"
#include "messenger/meshcore_mailbox.h"
#include "messenger/meshcore_messages.h"
#include "protocol/meshcore_session.h"
#include "scenes/meshcore_scene.h"

/* How often scenes get a tick event (used by the log view to refresh). */
#define MESHCORE_TICK_PERIOD_MS 500u

/* View ids registered with the ViewDispatcher. One per UI module, not per
 * scene — several scenes reuse the same module. */
typedef enum {
    MeshCoreViewSubmenu,
    MeshCoreViewWidget,
    MeshCoreViewTextBox,
    MeshCoreViewLoading,
} MeshCoreViewId;

/* What the node told us about itself. Filled by scene_connect from SELF_INFO
 * and DEVICE_INFO; the configurator editors read and modify it. */
typedef struct {
    bool valid;

    /* DEVICE_INFO */
    char model[MC_MAX_MODEL];
    char fw_ver[24];
    int8_t fw_ver_code;

    /* SELF_INFO */
    char name[MC_MAX_TEXT];
    uint32_t freq_khz; /* radio_freq, kHz — 869525 == 869.525 MHz */
    /* radio_bw is in Hz, not kHz: the reference client encodes set_radio's
     * bandwidth as bw_kHz * 1000, so 250 kHz goes over the wire as 250000.
     * The two radio fields genuinely use different scales. */
    uint32_t bw_hz;
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
    Loading* loading;

    /* Transport + protocol */
    MeshCoreLog* log;
    MeshCoreSession* session;
    MeshCoreNodeInfo node;

    /* Messenger */
    MeshCoreContacts contacts;
    MeshCoreMessages messages;
    MeshCoreMailbox* mailbox;
    /* Which conversation scene_chat is showing. Held as the peer's key rather
     * than a row number, because a contacts refresh can reorder the list. */
    uint8_t chat_peer[32];
    char chat_peer_name[MC_NAME_LEN + 1];

    /* Logger — owns its own session, on whichever port the node is on. */
    MeshCoreLogger* logger;

    /* Radio presets: built-ins always, card presets once Profiles has been
     * opened. Held on the app so the list survives moving between Profiles and
     * Apply without re-reading the card. */
    MeshCorePresetStore presets;
    size_t preset_index;
    /* One MeshCoreApplyResult per step; the enum lives in the apply scene
     * because nothing else has an opinion about it. */
    uint8_t apply_result[MeshCoreApplyStepCount];
    /* The node's clock, from CURR_TIME. Contact ages are relative to this and
     * not to the Flipper's RTC, because that is the timebase last_advert is
     * expressed in. Zero means "not read yet". */
    uint32_t node_time;

    /* Written by the session worker thread when the node pushes something
     * nobody asked for. Read by scenes; stage 2 turns this into real handling. */
    volatile uint32_t push_count;
    volatile uint8_t last_push_code;

    /* Scene named by the launch argument, opened once the dispatcher is
     * running rather than before it. Entering a scene that starts a worker
     * before view_dispatcher_run() means that worker posts into a queue nobody
     * is serving yet, which the logger scene reliably crashes on.
     * MeshCoreSceneNum means "no argument". */
    MeshCoreSceneId launch_scene;

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
