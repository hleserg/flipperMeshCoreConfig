#!/usr/bin/env python3
"""
meshcore_emulator.py — a fake MeshCore companion node.

Speaks the companion serial protocol well enough that a real client believes it
is talking to a radio. It exists so the Flipper app (Logger / Messenger /
Configurator) and meshlog.py can be exercised end to end with no hardware.

This is a test fixture, not a firmware simulator. It answers requests and emits
plausible unsolicited events; it does not model the mesh.

Every byte layout here is taken from primary sources, never guessed:
  * framing and command codes — meshcore-dev/MeshCore docs/companion_protocol.md
    and examples/companion_radio/MyMesh.cpp
  * reply layouts — cross-checked against the reference Python client
    (`pip install meshcore`), which is what meshlog.py drives

Framing, the thing that is easiest to get backwards:

    frame = [type:u8][len:u16 little-endian][payload:len]
    type 0x3C '<'  app  -> radio   (what a client sends us)
    type 0x3E '>'  radio -> app    (what we send back)

Both directions confirmed three ways: the firmware's ArduinoSerialInterface
(writeFrame emits '>', checkRecvFrame waits for '<'), the reference Python
client (sends b"\\x3c", scans for 0x3e), and the meshcore_c constants.

Usage:
    python meshcore_emulator.py --tcp 127.0.0.1:5000
    python meshcore_emulator.py --serial COM7 --model v4 --name ROVER
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import logging
import random
import struct
import time

LOG = logging.getLogger("emulator")

# ---------------------------------------------------------------- framing
FRAME_APP_TO_RADIO = 0x3C  # '<' — inbound, from the client
FRAME_RADIO_TO_APP = 0x3E  # '>' — outbound, from us

# The reference client rejects any frame claiming more than this.
MAX_FRAME_PAYLOAD = 300

# ---------------------------------------------------------------- commands
CMD_APP_START = 1
CMD_SEND_TXT_MSG = 2
CMD_SEND_CHANNEL_TXT_MSG = 3
CMD_GET_CONTACTS = 4
CMD_GET_DEVICE_TIME = 5
CMD_SET_DEVICE_TIME = 6
CMD_SEND_SELF_ADVERT = 7
CMD_SET_ADVERT_NAME = 8
CMD_SYNC_NEXT_MESSAGE = 10
CMD_SET_RADIO_PARAMS = 11
CMD_SET_RADIO_TX_POWER = 12
CMD_SET_ADVERT_LATLON = 14
CMD_GET_BATT_AND_STORAGE = 20
CMD_DEVICE_QUERY = 22
CMD_GET_CHANNEL = 31
CMD_GET_STATS = 56

# ---------------------------------------------------------------- responses
RESP_OK = 0
RESP_ERR = 1
RESP_CONTACTS_START = 2
RESP_CONTACT = 3
RESP_END_OF_CONTACTS = 4
RESP_SELF_INFO = 5
RESP_SENT = 6
RESP_CURR_TIME = 9
RESP_NO_MORE_MESSAGES = 10
RESP_BATT_AND_STORAGE = 12
RESP_DEVICE_INFO = 13
RESP_CHANNEL_INFO = 18
RESP_STATS = 24

STATS_CORE, STATS_RADIO, STATS_PACKETS = 0, 1, 2

# ---------------------------------------------------------------- models
MODELS = {
    "t114": "Heltec T114",
    "v4": "Heltec V4",
}

DEFAULT_CONTACTS = ["BASE", "REP1", "ROVER-M", "POCKET-S"]


def _cstr(text: str, width: int) -> bytes:
    """Fixed-width NUL-padded field, truncated if it does not fit."""
    raw = text.encode("utf-8")[:width]
    return raw + b"\0" * (width - len(raw))


def _pubkey_for(name: str) -> bytes:
    """Stable 32-byte identity per name, so restarts keep the same contacts."""
    return hashlib.sha256(("meshcore-emulator:" + name).encode()).digest()


class NodeState:
    """What our fake node currently believes about itself."""

    def __init__(self, name: str, model: str, contacts: list[str]):
        self.name = name
        self.model_key = model
        self.model = MODELS[model]
        self.public_key = _pubkey_for(name)
        self.started = time.monotonic()

        # Radio settings, in the units the wire actually uses: frequency in
        # kHz, bandwidth in Hz. They genuinely differ — the reference client
        # encodes set_radio as freq*1000 and bw*1000 from MHz and kHz.
        self.freq_khz = 869_525
        self.bw_hz = 250_000
        self.sf = 10
        self.cr = 5
        self.tx_power = 22
        self.max_tx_power = 30

        # Advertised position: somewhere unremarkable, so logs look real.
        self.lat = 55_751_244  # degrees x 1e6
        self.lon = 37_618_423

        self.contacts = list(contacts)

        # Counters that grow the way a working node's would.
        self.packets_recv = 0
        self.packets_sent = 0
        self.recv_errors = 0

    @property
    def uptime(self) -> int:
        return int(time.monotonic() - self.started)

    def battery_mv(self) -> int:
        """A slow, monotonic discharge curve — 4.15 V falling ~1 mV/s."""
        return max(3300, 4150 - self.uptime)


class CompanionEmulator:
    """Turns inbound command payloads into outbound reply payloads.

    Deliberately transport-agnostic: TCP and serial both just move bytes.
    """

    def __init__(self, state: NodeState):
        self.state = state

    # -- individual replies -------------------------------------------------

    def _self_info(self) -> bytes:
        s = self.state
        body = bytearray()
        body.append(1)  # adv_type
        body.append(s.tx_power)
        body.append(s.max_tx_power)
        body += s.public_key  # 32
        body += struct.pack("<i", s.lat)
        body += struct.pack("<i", s.lon)
        body.append(0)  # multi_acks
        body.append(0)  # adv_loc_policy
        body.append(0)  # telemetry_mode
        body.append(0)  # manual_add_contacts
        body += struct.pack("<I", s.freq_khz)
        body += struct.pack("<I", s.bw_hz)
        body.append(s.sf)
        body.append(s.cr)
        body += s.name.encode("utf-8")
        return bytes([RESP_SELF_INFO]) + bytes(body)

    def _device_info(self) -> bytes:
        s = self.state
        # fw_ver 9 is the lowest that carries the repeater flag; anything >= 3
        # brings the model and version strings, which is what identifies us.
        body = bytearray()
        body.append(9)  # fw_ver
        body.append(175)  # max_contacts / 2  -> 350
        body.append(20)  # max_channels
        body += struct.pack("<I", 123456)  # ble_pin
        body += _cstr("09-05-2026", 12)  # build date
        body += _cstr(s.model, 40)
        body += _cstr("v1.12.0", 20)
        body.append(0)  # repeat (fw_ver >= 9)
        return bytes([RESP_DEVICE_INFO]) + bytes(body)

    def _battery(self) -> bytes:
        # [12][millivolts u16][used_kb u32][total_kb u32] -- straight from
        # MyMesh.cpp. Note the reference client calls this "level"; it is
        # millivolts, not a percentage.
        return bytes([RESP_BATT_AND_STORAGE]) + struct.pack(
            "<HII", self.state.battery_mv(), 512, 8192
        )

    def _stats(self, kind: int) -> bytes:
        s = self.state
        if kind == STATS_CORE:
            return bytes([RESP_STATS, STATS_CORE]) + struct.pack(
                "<HIHB", s.battery_mv(), s.uptime, s.recv_errors, 0
            )
        if kind == STATS_RADIO:
            # Noise floor wanders a little, as a real receiver's does.
            noise = -108 + random.randint(-3, 3)
            return bytes([RESP_STATS, STATS_RADIO]) + struct.pack(
                "<hbbII", noise, -95, 20, s.uptime // 10, s.uptime // 4
            )
        if kind == STATS_PACKETS:
            return bytes([RESP_STATS, STATS_PACKETS]) + struct.pack(
                "<IIIIIII",
                s.packets_recv,
                s.packets_sent,
                s.packets_sent // 2,
                s.packets_sent - s.packets_sent // 2,
                s.packets_recv // 2,
                s.packets_recv - s.packets_recv // 2,
                s.recv_errors,
            )
        return bytes([RESP_ERR, 1])

    def _contact_frames(self) -> list[bytes]:
        """CONTACTS_START, one CONTACT each, then END_OF_CONTACTS."""
        s = self.state
        frames = [bytes([RESP_CONTACTS_START]) + struct.pack("<I", len(s.contacts))]

        now = int(time.time())
        for index, name in enumerate(s.contacts):
            body = bytearray()
            body += _pubkey_for(name)  # 32
            body.append(1)  # type
            body.append(0)  # flags
            body.append(0)  # out_path_len
            body += b"\0" * 64  # out_path
            body += _cstr(name, 32)
            # Heard from progressively longer ago, so "last seen" is not
            # identical for every row.
            body += struct.pack("<I", now - index * 600)
            body += struct.pack("<i", s.lat)
            body += struct.pack("<i", s.lon)
            body += struct.pack("<I", now)
            frames.append(bytes([RESP_CONTACT]) + bytes(body))

        frames.append(bytes([RESP_END_OF_CONTACTS]) + struct.pack("<I", now))
        return frames

    # -- dispatch -----------------------------------------------------------

    def handle(self, payload: bytes) -> list[bytes]:
        """Reply payloads for one command. Never empty: the reference client
        gives up after a few unanswered commands."""
        if not payload:
            return [bytes([RESP_ERR, 1])]

        cmd = payload[0]

        if cmd == CMD_APP_START:
            return [self._self_info()]

        if cmd == CMD_DEVICE_QUERY:
            return [self._device_info()]

        if cmd == CMD_GET_BATT_AND_STORAGE:
            return [self._battery()]

        if cmd == CMD_GET_STATS:
            kind = payload[1] if len(payload) > 1 else STATS_CORE
            return [self._stats(kind)]

        if cmd == CMD_GET_CONTACTS:
            return self._contact_frames()

        if cmd == CMD_GET_DEVICE_TIME:
            return [bytes([RESP_CURR_TIME]) + struct.pack("<I", int(time.time()))]

        if cmd == CMD_SET_DEVICE_TIME:
            return [bytes([RESP_OK])]

        if cmd == CMD_SYNC_NEXT_MESSAGE:
            # Stage 2 fills the mailbox; for now the node has nothing queued.
            return [bytes([RESP_NO_MORE_MESSAGES])]

        if cmd == CMD_SEND_SELF_ADVERT:
            return [bytes([RESP_OK])]

        if cmd == CMD_SET_ADVERT_NAME:
            self.state.name = payload[1:].decode("utf-8", "ignore")
            return [bytes([RESP_OK])]

        if cmd == CMD_SET_ADVERT_LATLON and len(payload) >= 9:
            self.state.lat, self.state.lon = struct.unpack("<ii", payload[1:9])
            return [bytes([RESP_OK])]

        if cmd == CMD_SET_RADIO_PARAMS and len(payload) >= 11:
            freq, bw, sf, cr = struct.unpack("<IIBB", payload[1:11])
            self.state.freq_khz, self.state.bw_hz = freq, bw
            self.state.sf, self.state.cr = sf, cr
            return [bytes([RESP_OK])]

        if cmd == CMD_SET_RADIO_TX_POWER and len(payload) >= 5:
            self.state.tx_power = struct.unpack("<I", payload[1:5])[0]
            return [bytes([RESP_OK])]

        if cmd == CMD_GET_CHANNEL:
            index = payload[1] if len(payload) > 1 else 0
            body = bytes([index]) + _cstr("public", 32) + b"\0" * 16
            return [bytes([RESP_CHANNEL_INFO]) + body]

        LOG.info("unhandled command 0x%02X, answering OK", cmd)
        return [bytes([RESP_OK])]


class FrameCodec:
    """Reassembles inbound frames from a byte stream."""

    def __init__(self):
        self.buffer = bytearray()

    @staticmethod
    def encode(payload: bytes) -> bytes:
        if len(payload) > MAX_FRAME_PAYLOAD:
            raise ValueError(f"payload too large: {len(payload)}")
        return bytes([FRAME_RADIO_TO_APP]) + struct.pack("<H", len(payload)) + payload

    def feed(self, data: bytes) -> list[bytes]:
        """Append bytes, return every complete payload now available."""
        self.buffer += data
        out = []

        while True:
            # Resync: drop anything before a plausible lead byte, the way a
            # real node does when console noise shares the line.
            start = self.buffer.find(bytes([FRAME_APP_TO_RADIO]))
            if start < 0:
                self.buffer.clear()
                return out
            if start > 0:
                del self.buffer[:start]

            if len(self.buffer) < 3:
                return out

            length = struct.unpack("<H", self.buffer[1:3])[0]
            if length > MAX_FRAME_PAYLOAD:
                del self.buffer[:1]  # bogus length: skip the lead and resync
                continue
            if len(self.buffer) < 3 + length:
                return out

            out.append(bytes(self.buffer[3 : 3 + length]))
            del self.buffer[: 3 + length]


async def serve_tcp(host: str, port: int, emulator: CompanionEmulator) -> None:
    async def on_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        peer = writer.get_extra_info("peername")
        LOG.info("client connected: %s", peer)
        codec = FrameCodec()
        try:
            while True:
                data = await reader.read(4096)
                if not data:
                    break
                for payload in codec.feed(data):
                    LOG.debug("<- cmd 0x%02X (%d bytes)", payload[0], len(payload))
                    for reply in emulator.handle(payload):
                        LOG.debug("-> resp 0x%02X (%d bytes)", reply[0], len(reply))
                        writer.write(FrameCodec.encode(reply))
                    await writer.drain()
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            LOG.info("client gone: %s", peer)
            writer.close()

    server = await asyncio.start_server(on_client, host, port)
    LOG.info("listening on %s:%d", host, port)
    async with server:
        await server.serve_forever()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Fake MeshCore companion node")
    transport = parser.add_mutually_exclusive_group(required=True)
    transport.add_argument("--tcp", metavar="HOST:PORT", help="e.g. 127.0.0.1:5000")
    transport.add_argument("--serial", metavar="PORT", help="e.g. COM7 (stage 3)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--model", choices=sorted(MODELS), default="t114")
    parser.add_argument("--name", default="EMU-1")
    parser.add_argument(
        "--contacts",
        default=",".join(DEFAULT_CONTACTS),
        help="comma-separated fake contact names",
    )
    parser.add_argument("--verbose", "-v", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%H:%M:%S",
    )

    contacts = [c.strip() for c in args.contacts.split(",") if c.strip()]
    state = NodeState(args.name, args.model, contacts)
    emulator = CompanionEmulator(state)

    LOG.info("pretending to be %s (%s), contacts: %s", state.name, state.model, contacts)

    if args.serial:
        LOG.error("serial transport lands in stage 3; use --tcp for now")
        return 2

    host, _, port = args.tcp.partition(":")
    try:
        asyncio.run(serve_tcp(host or "127.0.0.1", int(port or 5000), emulator))
    except KeyboardInterrupt:
        LOG.info("stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
