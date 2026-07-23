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
- [x] 2 — receive: `MSG_WAITING` wakes a mailbox worker that drains with
      `SYNC_NEXT_MESSAGE`; messages land in a RAM ring and `scene_chat` renders
      the conversation. Also drains on a 5 s timer so mail queued before we
      connected is not stranded.
- [ ] 3 — send: `scene_compose` on `TextInput` → `SEND_TXT_MSG`
- [ ] 4 — history on SD (`/ext/apps_data/meshcore_cfg/messages/`) + incoming
      notifications (vibro + line)
- [ ] `scene_channels` — public channels via `GET_CHANNEL`

## Logger (field link testing)

Radio on the nodes. Both Flipper UARTs are universal — role and hardware are
discovered per node, not fixed per port.

- [x] 1 — companion node on the USART, `RX_LOG_DATA` (0x88) → `rx_log.csv` on
      SD, one directory per session, every row synced as written
- [ ] 2 — companion telemetry: `GET_STATS` core/radio/packets → `telemetry.csv`
- [ ] 3 — ping (RTT via ACK) → `ping.csv`; adverts and messages → `events.csv`
- [ ] 4 — auto-detect interface (companion|repeater) and hardware (T114|V4),
      fill `MeshCoreLogNode`, tag rows with role/hw
- [ ] 5 — repeater branch: text CLI handler, stats polling → `telemetry.csv`
      with `role=repeater`
- [ ] 6 — second UART (LPUART) and multiple nodes at once, mixed roles
- [ ] 7 — UI: live SNR/RTT, a button to drop a `mark` into `events.csv`,
      pass/fail colouring against the acceptance thresholds

Interactions this created with earlier work, already handled:

- The UART layer now takes a port id, and the expansion-service disable is
  reference counted — with two ports open it must be disabled once and restored
  only when the last closes.
- `meshcore_link_poll` used to **discard** frames meshcore_c could not decode.
  `RX_LOG_DATA` is exactly such a frame, so the Logger would have received
  nothing. Undecoded frames are now delivered with their raw payload.
- The session event callback carries the raw payload for the same reason.

## Node emulator (`tools/node-emulator/`) — next up

A Python fake companion node, so the whole app can be exercised before any
board arrives. This is the highest-leverage item on the list: it converts most
of "waiting for hardware" below into something testable today.

- [ ] 1 — protocol core + TCP transport: framing and replies for self-info,
      get_bat, stats radio/packets, get_contacts.
      *Done when:* the pip `meshcore` client reads self-info and battery over TCP.
- [ ] 2 — unsolicited events + ping: adverts, `rx_log` with drifting SNR,
      `send_msg` → ACK with delay and configurable loss, incoming messages.
      *Done when:* `meshlog.py --tcp 127.0.0.1:5000` fills rx_log/telemetry/ping.
- [ ] 3 — serial transport, `--model`, `--scenario`; ESP32-S2 devboard as a
      dumb USB↔UART bridge.
      *Done when:* a real Flipper logs the emulator over the wire.
- [ ] `HANDOFF.md` — plain-language, numbered, split into "already done" and
      "your turn", ending in a symptom → cause → fix table.

**Blocked on input:** `meshlog.py` has not been provided, and stage 2's gate is
defined by it. It also decides the `ts` column format the Logger writes.

The emulator must be byte-correct or it proves nothing — the format comes from
`docs/companion_protocol.md` and the firmware sources, never from guesswork.

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
- [!] **Loopback check — the highest-value pre-node test.** Needs only a
      jumper between pins 13 and 14. Connect should report *"Node refused the
      handshake"* and the serial log should show identical `TX`/`RX` lines.
      That counterintuitive message is the success signal.

      It now covers more than when it was first proposed. The echo of
      `APP_START` comes back with lead byte `0x3C`, so the session worker
      polls it, assembles it, parses code `0x01` as `MC_RESP_ERR`, routes it
      as a Reply, sets the event flag, and the scene worker completes and
      renders — which is exactly the threaded machinery that host tests
      cannot reach. As a bonus the 5 s mailbox drain sends
      `SYNC_NEXT_MESSAGE` (command code 10), whose echo parses as
      `MC_RESP_NO_MORE_MESSAGES` (also code 10), so the multi-code routing
      and the drain loop's exit condition get exercised too.

      A hang or a crash here is a synchronisation bug that nothing else in the
      current setup would surface.
- [!] Whether the node accepts a runtime role change at all (see the Role
      caveat in AGENTS.md) — decides if `scene_role` is an editor or read-only
- [!] Confirm the Heltec V4 works on stock `heltec_v4_companion_radio_usb`
- [x] ~~Check that the log view's tick refresh fights manual scrolling~~ — it
      did. Reported from the device and fixed: timed redraws reset scroll
      position in both the Logger and the Serial log.
- [!] Logger 1: a walk near the mesh fills `rx_log.csv` with SNR/RSSI rows
- [!] Logger: confirm the timestamp format matches what `meshlog.py` wrote.
      Currently ISO-8601 without a zone (`2026-07-23T12:34:56`), chosen blind —
      **`meshlog.py` has not been seen**, so if it used epoch seconds the
      column needs changing. One function, `meshcore_logger_timestamp()`.
- [!] Logger: partial gate that needs no node — open Logger on the device and
      confirm the session directory and `rx_log.csv` (with its header) appear
      on the SD card. Verifiable over the Flipper CLI with `storage list`.

## Known gaps

- `scene_chat` right-aligns outgoing messages with `\er` but cannot invert
  them: the text scroll element has no per-line colour. Real inversion needs a
  custom view with its own draw callback — worth doing in stage 3, when there
  are outgoing messages to look at.
- `MC_PUSH_NEW_ADVERT` carries a full contact record and could refresh the
  contact list live. Currently ignored; contacts only update on entering the
  scene.
- Delivery confirmation (`MC_PUSH_SEND_CONFIRMED`) is not tracked yet — needed
  once stage 3 can send, to tell "sent" from "delivered".
- **Stale reply after a timeout.** The companion protocol puts no request id on
  the wire, so a reply that arrives after its request gave up cannot be told
  from the next request's reply. If request N times out and its answer lands
  while request N+1 is armed with the same expected codes, N+1 completes early
  on the wrong frame. Back-to-back mailbox drains are the realistic case. Not
  fixed speculatively — a sequence tag would not help without wire-level
  correlation. **If bring-up on a flaky link shows duplicated or misattributed
  messages, look here first.**
- The session's threaded mechanism (arming the slot, the event-flag
  clear/wait/set dance) has no automated coverage — only the pure routing
  policy does. The loopback jumper test is the cheapest way to exercise it; see
  "Waiting for hardware".
