# Task list

Working order and current state. Updated as work lands; new requests are
appended unless they change what is already planned.

Legend: `[x]` done · `[~]` in progress · `[ ]` not started · `[!]` blocked

## Gate policy

Two gates, and they are not the same thing:

- **Build gate** — `ufbt` builds clean, and `pwsh test/run.ps1` is green. This
  is enforced before anything is called done.
- **Node gate** — verified against a real MeshCore node. **Currently
  unreachable: the nodes are in transit from China.** Everything needing one is
  collected under "Waiting for hardware" below and will be run as a batch when
  they arrive.

Remote UI driving is not available either: the Flipper CLI accepts
`input send` but the events never reach a running app, confirmed with debug
mode on. So scenes cannot be exercised from here at all.

## Configurator

- [x] 1 — ufbt skeleton: manifest, entry point, SceneManager, static menu
- [x] 2 — UART layer + `scene_connect`: handshake, model/firmware, serial log
- [x] 3 — vendor `meshcore_c`, first real command *(folded into 2: reading
      SELF_INFO **is** the config read, and hand-rolling a frame parser was
      ruled out, so the library had to land first)*
- [ ] 4 — `scene_radio` / `scene_identity` / `scene_role` on `VariableItemList`
- [ ] 5 — `scene_profiles` (JSON from SD) + `scene_apply` with a result checklist

Menu entries for Radio / Identity / Role / Profiles / Send advert exist but are
inert until steps 4 and 5.

## Messenger

Radio lives on the external node; the Flipper is the client.

- [x] 1 — `scene_contacts`: long-lived session worker, then the contact list
      *(architecture pulled forward deliberately — the old request/response
      model could not hear an unsolicited push, so stage 2 would have forced a
      rewrite)*
- [ ] 2 — receive: handle the `MSG_WAITING` push, drain with
      `SYNC_NEXT_MESSAGE`, show in `scene_chat`
- [ ] 3 — send: `scene_compose` on `TextInput` → `SEND_TXT_MSG`
- [ ] 4 — history on SD (`/ext/apps_data/meshcore_cfg/messages/`) + incoming
      notifications (vibro + line)
- [ ] `scene_channels` — public channels via `GET_CHANNEL`

## Infrastructure

- [x] Host test suite for the protocol layer (`test/`), Zig toolchain
- [x] Flipper firmware brought to Unleashed `unlshd-089` (was `080e`, API 79 →
      87, the app could not have loaded on the old one)
- [x] Build against the Unleashed SDK, documented both directions
- [x] `application.fam` lists sources per directory so `test/` cannot leak into
      the firmware

## Waiting for hardware

To run in one batch once a node is on the bench:

- [!] Configurator: `Connect` against a real node — model, firmware, radio
      settings render correctly
- [!] Messenger 1: contact list shows real peers with sane "last seen" ages
- [!] Messenger 2: a message sent from another node/phone appears on the Flipper
- [!] Messenger 3: a message typed on the Flipper arrives at another node
- [!] Loopback sanity check (needs only a jumper between pins 13 and 14):
      Connect should report *"Node refused the handshake"* and the serial log
      should show identical `TX`/`RX` lines. That counterintuitive message is
      the success signal — it proves the whole chain short of node-specific
      parsing.
- [!] Whether the node accepts a runtime role change at all (see the Role
      caveat in AGENTS.md) — decides if `scene_role` is an editor or read-only
- [!] Confirm the Heltec V4 works on stock `heltec_v4_companion_radio_usb`
- [!] Check that the log view's tick refresh does not fight manual scrolling
