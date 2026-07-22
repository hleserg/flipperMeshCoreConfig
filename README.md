# MeshCore Config

A [Flipper Zero](https://flipperzero.one/) app for configuring
[MeshCore](https://meshcore.co.uk/) nodes over the hardware UART — radio
parameters, node identity, role, profiles from the SD card, and self-adverts,
without a phone or a laptop.

By **Greyrock Labs**. MIT licensed.

> **Status: work in progress.** Step 1 of 5 — the app skeleton and main menu.
> The menu is currently static; the UART and protocol layers land next.

## Why

MeshCore nodes are normally configured from a phone over BLE or from a desktop
over USB. In the field neither is convenient. The Flipper already lives in your
pocket, has a UART on its GPIO header and runs off its own battery — three
wires and you can retune a node standing in the rain.

## Wiring

| Flipper | | Node |
| --- | --- | --- |
| pin 13 — TX | → | RX |
| pin 14 — RX | ← | TX |
| pin 18 — GND | — | GND |

115200 8N1. Both sides are 3.3 V logic, so no level shifter is needed.

The Flipper is **not** a USB host. It cannot talk to a node over USB-CDC — the
node has to expose the companion protocol on a real UART. That is the whole
point of the section below.

## Supported node firmware

This app speaks the MeshCore **companion serial protocol**. The node must be
running a *companion* firmware build whose serial interface is bound to a
**hardware UART**, not to USB-CDC or BLE.

### Heltec V4 (ESP32-S3)

Expected to work with the stock `heltec_v4_companion_radio_usb` build. That
build binds the companion interface to `Serial`, which on this board is UART0
(GPIO43 TX / GPIO44 RX) — the same port the on-board USB-serial bridge uses, so
it is reachable directly from the Flipper's header.

### Heltec T114 (nRF52840) — ⚠️ requires a custom firmware build

**The stock T114 companion firmware will not work with this app.**

On nRF52 targets, MeshCore's `examples/companion_radio/main.cpp` offers only two
options: BLE (when `BLE_PIN_CODE` is defined) or `serial_interface.begin(Serial)`
— and on the Adafruit nRF52 Arduino core `Serial` is **USB-CDC**. There is no
`SERIAL_RX` hardware-UART branch on this platform, unlike ESP32 and RP2040.

To use a T114 you must rebuild its companion firmware with the serial interface
bound to `Serial1` on the board's exposed UART pins, i.e. the nRF52 branch
changed from `serial_interface.begin(Serial)` to a hardware `Serial1` started at
115200.

**Producing that firmware is out of scope for this app** — it is a prerequisite
you provide. This repository ships no node firmware.

### Other boards

Anything running a MeshCore companion build on a hardware UART at 115200 8N1
should work; only the two boards above are actively targeted.

## Features

Planned, in the order they are being built:

- **Connect** — open the UART, handshake, show node model and firmware version
- **Radio** — frequency, bandwidth, spreading factor, coding rate, TX power
- **Identity** — node name as it appears in adverts
- **Role** — companion / repeater / room server *(see the caveat below)*
- **Profiles** — apply a saved `*.json` profile from the SD card
- **Send advert** — trigger a zero-hop or flood self-advert

Profiles live in `/ext/apps_data/meshcore_cfg/profiles/` and look like this:

```json
{
  "name": "uk-868-fast",
  "freq": 869525,
  "bw": 250,
  "sf": 10,
  "cr": 5,
  "tx": 22,
  "role": "companion",
  "advert": true
}
```

`freq` is in kHz, `bw` in kHz, `tx` in dBm.

> **Caveat on Role:** in MeshCore, companion / repeater / room server are
> separate firmware builds rather than a runtime setting, and the companion
> protocol has no "set role" command. What this app can actually change at
> runtime is being verified against real hardware; the menu entry may end up
> read-only.

## Building

Requires [`ufbt`](https://pypi.org/project/ufbt/):

```sh
pip install --user ufbt

git clone https://github.com/hleserg/flipperMeshCoreConfig
cd flipperMeshCoreConfig

ufbt              # build -> dist/meshcore_cfg.fap
ufbt launch       # build, upload to a connected Flipper and run
```

Or copy `dist/meshcore_cfg.fap` to `/ext/apps/GPIO/` on the SD card by hand.

## Credits

- Companion protocol client: [SH3D/meshcore_c](https://github.com/SH3D/meshcore_c) (MIT)
- Protocol and firmware: [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore) (MIT)
