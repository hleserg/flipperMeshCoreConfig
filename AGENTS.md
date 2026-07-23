# AGENTS.md — MeshCore Config (Flipper Zero FAP)

Context-first brief for anyone (human or agent) picking this repo up.

## What this is

A Flipper Zero application that configures a **MeshCore** node over the
Flipper's **hardware UART**: radio parameters, node identity, role, applying
profiles from the SD card, and triggering a self-advert.

Brand: **Greyrock Labs**. License: MIT.

Target nodes: **Heltec T114** (nRF52840) and **Heltec V4** (ESP32-S3).

## Stack

| Layer | What |
| --- | --- |
| Build | `ufbt` (Flipper micro build tool), official firmware SDK |
| Language | C (C99 for the vendored protocol code, Flipper's C dialect elsewhere) |
| UI | `SceneManager` + `ViewDispatcher`, `Submenu` and `VariableItemList` modules |
| Transport | `furi_hal_serial` on `FuriHalSerialIdUsart`, 115200 8N1 |
| Protocol | [SH3D/meshcore_c](https://github.com/SH3D/meshcore_c) — single-file C99 companion-protocol client, vendored (MIT) |

## Build and run

```sh
ufbt              # build the .fap into dist/
ufbt launch       # build, upload to a USB-connected Flipper and start the app
ufbt cli          # serial CLI to the Flipper (log output)
ufbt update       # pull/refresh the firmware SDK
```

`ufbt` comes from PyPI: `pip install ufbt`.

### Target firmware: Unleashed

**This app is built against the Unleashed SDK, not the official one.** Point
ufbt at the Unleashed index once per machine, before the first build:

```sh
ufbt update --index-url=https://up.unleashedflip.com/directory.json --channel=release
```

To go back to official firmware:

```sh
ufbt update --index-url=https://update.flipperzero.one/firmware/directory.json --channel=release
```

This matters because a FAP records the API version it was compiled against and
the firmware refuses to load one that asks for more than it provides. Official
1.4.3 is API **87.1**; Unleashed `unlshd-089` is API **87.8**. Same major, so a
FAP built against *official* still loads on Unleashed — but one built against
Unleashed will **not** load on official firmware. Nothing catches this at build
time; it fails on the device.

The choice lives in `~/.ufbt/current/ufbt_state.json`, which a repository
cannot pin — so check it whenever a build behaves unexpectedly:

```sh
cat ~/.ufbt/current/ufbt_state.json
```

The code itself uses no Unleashed-specific API, so it compiles against either.

There are no unit tests; the build itself plus a run on hardware is the gate.
**Do not call a step done until `ufbt` builds clean.**

## Layout

```
application.fam            app manifest (appid, entry point, icon, category)
meshcore_cfg.h/.c          MeshCoreApp state, ViewDispatcher/SceneManager wiring, entry point
meshcore_log.h/.c          mutex-guarded in-memory traffic log shown by scene_log
meshcore_10px.png          10x10 app icon
uart/
  meshcore_uart.h/.c       furi_hal_serial wrapper; bytes only, no protocol
protocol/
  meshcore_c/              vendored upstream library — do not patch, see VENDOR.md
  meshcore_link.h/.c       binds meshcore_c to the UART layer; blocking request/poll
scenes/
  meshcore_scene_config.h  X-macro list — the single place scenes are registered
  meshcore_scene.h/.c      generated scene enum + handler tables
  meshcore_scene_menu.c    main menu
  meshcore_scene_connect.c handshake on a worker thread, shows model/fw/radio
  meshcore_scene_log.c     passthrough hex log, refreshed on the scene tick
```

Planned (not yet present):

```
profiles/                  JSON profile loading from the SD card
```

Adding a scene = one line in `scenes/meshcore_scene_config.h` +
`scenes/meshcore_scene_<name>.c` with the three handlers. `sources` is left at
the fbt default (`*.c*`, globbed recursively), so new files are picked up
automatically.

## Wiring

| Flipper | Direction | Node |
| --- | --- | --- |
| pin 13 (TX, PB6) | → | node RX |
| pin 14 (RX, PB7) | ← | node TX |
| pin 18 (GND) | — | node GND |

115200 8N1. Flipper GPIO is 3.3 V, same as both target nodes — no level
shifter needed. The Flipper has **no USB host**, so the node must expose the
companion protocol on a *hardware* UART, not on USB-CDC. See below.

Pins 13/14 are USART1 (`FuriHalSerialIdUsart`); pins 15/16 are LPUART. We use
USART.

**Gotcha for the UART layer:** the same USART backs the Flipper's expansion
module service. Before `furi_hal_serial_control_acquire(FuriHalSerialIdUsart)`
the app must disable it, and re-enable it on exit:

```c
Expansion* expansion = furi_record_open(RECORD_EXPANSION);
expansion_disable(expansion);
/* ... acquire / init / use / deinit / release the serial handle ... */
expansion_enable(expansion);
furi_record_close(RECORD_EXPANSION);
```

## Companion serial protocol

Wire format, verified against the MeshCore firmware
(`src/helpers/ArduinoSerialInterface.cpp`):

```
frame = [type:u8][len:u16 little-endian][payload:len bytes]

type 0x3C = '<'   app -> radio    (what this FAP sends)
type 0x3E = '>'   radio -> app    (what this FAP receives)

payload[0] = command code (outbound) / response or push code (inbound)
```

> **Note:** the direction of the two marker bytes is easy to get backwards.
> The firmware's `writeFrame()` emits `'>'` and its `checkRecvFrame()` waits
> for `'<'` — from the *node's* point of view. So the **FAP sends `'<'` and
> receives `'>'`**. `meshcore_c` already encodes this correctly
> (`MC_FRAME_APP_TO_RADIO == 0x3C`). Do not "fix" it.

`meshcore_c` does no I/O and no dynamic allocation — it is a frame assembler
(`mc_rx_feed` / `mc_rx_poll`), a set of payload builders (`mc_cmd_*`) that
write into a caller-owned buffer, `mc_frame_encode` to wrap a payload, and
`mc_parse` to decode one received payload into an `mc_event_t`. That maps
cleanly onto `furi_hal_serial_async_rx` + `furi_hal_serial_tx`.

Commands this app needs:

| Purpose | Builder | Reply |
| --- | --- | --- |
| Handshake / read current config | `mc_cmd_app_start` | `MC_RESP_SELF_INFO` → `mc_self_info_t` (name, `radio_freq/bw/sf/cr`, `tx_power`, `type`) |
| Model + firmware version | `mc_cmd_device_query` | `MC_RESP_DEVICE_INFO` → `mc_device_info_t` (`model`, `ver`, `fw_ver`) |
| Radio params | `mc_cmd_set_radio_params(freq_khz, bw, sf, cr)` | `MC_RESP_OK` / `MC_RESP_ERR` |
| TX power | `mc_cmd_set_tx_power(dbm)` | `MC_RESP_OK` / `MC_RESP_ERR` |
| Node name | `mc_cmd_set_advert_name(name)` | `MC_RESP_OK` / `MC_RESP_ERR` |
| Self advert | `mc_cmd_send_self_advert(MC_ADVERT_ZERO_HOP \| MC_ADVERT_FLOOD)` | `MC_RESP_OK` / `MC_RESP_ERR` |

Note `mc_cmd_set_radio_params` takes frequency in **kHz** (the header calls the
argument `freq_hz_x1000`).

**Open question — role switching.** The companion protocol has no
"set role" command: in MeshCore, companion / repeater / room server are
separate firmware builds (see the `*_repeater`, `*_room_server`,
`*_companion_radio_*` PlatformIO envs). `mc_device_info_t.repeat` exists on
fw_ver ≥ 9, and `mc_cmd_send_cmd` can push a CLI string to a node. Before
building `scene_role`, confirm on hardware what a node actually accepts.

## Node firmware requirements

The node must run a **companion** build whose `ArduinoSerialInterface` is bound
to a hardware serial port.

**Heltec V4 (ESP32-S3)** — stock `heltec_v4_companion_radio_usb` calls
`serial_interface.begin(Serial)`. With no `ARDUINO_USB_CDC_ON_BOOT` override,
`Serial` is UART0 (GPIO43 TX / GPIO44 RX), the port behind the on-board
USB-serial bridge. Expected to work as shipped; confirm on hardware.

**Heltec T114 (nRF52840)** — **does not work as shipped.** On the nRF52 branch
of `examples/companion_radio/main.cpp` there is no `SERIAL_RX` case: it is
either BLE (`BLE_PIN_CODE`) or `serial_interface.begin(Serial)`, and on the
Adafruit nRF52 core `Serial` is **USB-CDC**. The Flipper cannot act as a USB
host, so the node must be rebuilt with the companion interface bound to
`Serial1` on the T114's exposed UART pins. Building that firmware is **out of
scope for this FAP** — it is a documented prerequisite.

## Conventions

- Prefix everything `meshcore_` / `MeshCore` to keep the global namespace clean.
- Classic Flipper look: inverted selected row (that is what `Submenu` and
  `VariableItemList` already do — don't hand-roll widgets).
- `VariableItemList` for value pickers; it renders the `‹ ›` arrows for free.
- Comments explain *why*, not *what*.

### Threading

Three contexts, and mixing them is the main way to break this app:

- **ISR** — `meshcore_uart_rx_isr` only. Drains the peripheral into a stream
  buffer and returns. Never allocate, never block, never touch a view.
- **Worker thread** — everything in `protocol/` blocks, so every `mc_cmd_*` /
  `meshcore_link_request` call belongs here. A worker must not call any GUI
  function; it reports back with `view_dispatcher_send_custom_event()`, which
  is safe to call from another thread. See `scene_connect` for the shape:
  `furi_thread_alloc_ex` in `on_enter`, join and free in `on_exit`.
- **GUI thread** — scene handlers. Never call into `protocol/` from here; a
  silent node would freeze the UI for the request timeout.

`TextBox` stores the `const char*` it is given rather than copying it, and the
GUI service redraws from that pointer on its own thread. So the string behind a
live `TextBox` must never be mutated — `scene_log` keeps two `FuriString`
buffers in `MeshCoreApp` and swaps between them. `Widget` elements do copy, so
a stack buffer is fine there.

## Working order

Incremental; each step must build under `ufbt` before the next begins.

1. ufbt skeleton: manifest, entry point, SceneManager, static `scene_menu`. ← **done**
2. UART layer + `scene_connect`: open the port, read self-info, show model and
   firmware version, plus a passthrough log for debugging. ← **done**
3. Vendor `meshcore_c`, wire the first real command (read config). ← **done**
   (folded into step 2: reading self-info *is* the config read, and the task
   forbids hand-rolling a frame parser, so the library had to land first.
   `MeshCoreNodeInfo` in `meshcore_cfg.h` is the config the editors will edit.)
4. `scene_radio` / `scene_identity` / `scene_role` editors on `VariableItemList`.
5. `scene_profiles` (JSON from `/ext/apps_data/meshcore_cfg/profiles/`) and
   `scene_apply` (send commands, show a result checklist).
