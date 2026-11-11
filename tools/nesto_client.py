"""A small Home Assistant stand-in that talks to a Nesto panel.

It does what the ha-nesto integration will do: pair, authenticate, send the
synchronisation messages and exchange messages over the encrypted session. The
end-to-end tests use it, and it is handy by hand against the simulator:

    python3 tools/nesto_client.py 127.0.0.1 pair
    python3 tools/nesto_client.py 127.0.0.1 connect

`pair` asks for the code the panel shows and saves the key to a file
(nesto.key by default); `connect` uses that key, synchronises, and prints what
the panel sends until interrupted; `demo` serves a made-up room, carrying out
the panel's intents the way Home Assistant would, so the screens can be tried.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
import struct
import sys
from collections.abc import Awaitable, Callable
from datetime import datetime, timedelta
from pathlib import Path

import nesto_protocol as p

DEFAULT_PORT = 6170


class PanelError(Exception):
    """The panel answered a request with an error, or broke the protocol."""

    def __init__(self, message: str, code: str | None = None) -> None:
        super().__init__(message)
        self.code = code


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode()


def unb64(text: str) -> bytes:
    return base64.b64decode(text, validate=True)


class NestoClient:
    """One connection to a panel."""

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self._reader = reader
        self._writer = writer
        self._next_id = 1
        self._to_panel: tuple[bytes, int] | None = None  # key, counter
        self._from_panel: tuple[bytes, int] | None = None
        self.hello: dict = {}
        self.events: list[dict] = []  # messages received while waiting for a result

    @classmethod
    async def connect(cls, host: str, port: int = DEFAULT_PORT, timeout: float = 10) -> NestoClient:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout)
        client = cls(reader, writer)
        client.hello = await asyncio.wait_for(client.receive(), timeout)
        if client.hello.get("t") != "hello":
            raise PanelError(f"expected hello, got {client.hello}")
        return client

    async def close(self) -> None:
        self._writer.close()
        try:
            await self._writer.wait_closed()
        except ConnectionError:
            pass

    # Frames and messages -------------------------------------------------

    async def send(self, message: dict) -> None:
        payload = json.dumps(message, separators=(",", ":")).encode()
        if self._to_panel:
            key, counter = self._to_panel
            self._writer.write(p.seal(key, counter, payload))
            self._to_panel = (key, counter + 1)
        else:
            self._writer.write(p.frame(payload))
        await self._writer.drain()

    async def send_raw(self, data: bytes) -> None:
        self._writer.write(data)
        await self._writer.drain()

    async def receive(self) -> dict:
        header = await self._reader.readexactly(4)
        (length,) = struct.unpack(">I", header)
        if not 0 < length <= p.MAX_FRAME:
            raise PanelError(f"bad frame length {length}")
        body = await self._reader.readexactly(length)
        if self._from_panel:
            key, counter = self._from_panel
            body = p.open_frame(key, counter, header + body)
            self._from_panel = (key, counter + 1)
        return json.loads(body)

    async def request(self, message: dict, timeout: float = 10) -> dict:
        """Sends a request and returns its result, keeping other messages in events."""
        request_id = self._next_id
        self._next_id += 1
        await self.send({**message, "id": request_id})
        while True:
            reply = await asyncio.wait_for(self.receive(), timeout)
            if reply.get("t") == "result" and reply.get("re") == request_id:
                if not reply.get("ok"):
                    error = reply.get("error", {})
                    raise PanelError(error.get("message", "error"), error.get("code"))
                return reply
            self.events.append(reply)

    async def wait_for(self, message_type: str, timeout: float = 10) -> dict:
        for i, event in enumerate(self.events):
            if event.get("t") == message_type:
                return self.events.pop(i)
        while True:
            message = await asyncio.wait_for(self.receive(), timeout)
            if message.get("t") == message_type:
                return message
            self.events.append(message)

    # Pairing and authentication (PROTOCOL.md sections 6 and 7) -----------

    async def pair(self, confirm: Callable[[str], Awaitable[bool]]) -> bytes:
        """Pairs and returns the key. `confirm` gets the code HA computed and
        returns whether it matches the one the panel shows."""
        private = os.urandom(32)
        ha_pub = p.public_key(private)
        begun = await self.request({"t": "pair_begin", "pub": b64(ha_pub)})
        panel_pub = unb64(begun["pub"])
        shared = p.shared_secret(private, panel_pub)
        code = p.pairing_code(ha_pub, panel_pub, shared)
        if not await confirm(code):
            await self.send({"t": "pair_cancel"})
            raise PanelError("the code didn't match", "cancelled")
        key = p.pairing_key(shared, ha_pub, panel_pub)
        confirmed = await self.request(
            {"t": "pair_confirm", "mac": b64(p.confirm_mac(key, p.ROLE_HA))}
        )
        if unb64(confirmed["mac"]) != p.confirm_mac(key, p.ROLE_PANEL):
            raise PanelError("the panel's confirmation is wrong")
        return key

    async def authenticate(self, key: bytes) -> None:
        panel_nonce = unb64(self.hello["nonce"])
        ha_nonce = os.urandom(16)
        panel_id = self.hello["panel_id"]
        mac = p.auth_mac(key, p.ROLE_HA, panel_nonce, ha_nonce, panel_id)
        result = await self.request({"t": "auth", "nonce": b64(ha_nonce), "mac": b64(mac)})
        expected = p.auth_mac(key, p.ROLE_PANEL, panel_nonce, ha_nonce, panel_id)
        if unb64(result["mac"]) != expected:
            raise PanelError("the panel's authentication MAC is wrong")
        to_panel, from_panel = p.session_keys(key, panel_nonce, ha_nonce)
        self._to_panel = (to_panel, 0)
        self._from_panel = (from_panel, 0)

    async def synchronise(
        self,
        title: str = "Test room",
        items: list[dict] | None = None,
        states: dict[str, dict] | None = None,
        config: dict | None = None,
    ) -> None:
        """Sends the four synchronisation messages (section 8) and waits for ready.
        `config` is the body of the config message (section 12)."""
        now = datetime.now().astimezone()
        offset = now.utcoffset() or timedelta()
        await self.send(
            {
                "t": "time",
                "utc_ms": int(now.timestamp() * 1000),
                "utc_offset_min": int(offset.total_seconds() // 60),
            }
        )
        await self.send({"t": "config", **(config or {})})
        await self.send({"t": "layout", "rev": 1, "title": title, "items": items or []})
        await self.send({"t": "states", "full": True, "items": states or {}})
        await self.wait_for("ready")


# A demo room ------------------------------------------------------------------

DEMO_TITLE = "Living room"

DEMO_ITEMS = [
    {
        "key": "l1",
        "kind": "light",
        "name": "Ceiling",
        "features": {
            "brightness": True,
            "color_temp": {"min_k": 2200, "max_k": 6500},
            "color": True,
            "effects": ["Candle", "Fireplace", "Sunrise"],
        },
    },
    {"key": "l2", "kind": "light", "name": "Floor lamp", "features": {"brightness": True}},
    {
        "key": "c1",
        "kind": "climate",
        "name": "Radiator",
        "features": {
            "target": "single",
            "min": 5,
            "max": 30,
            "step": 0.5,
            "unit": "\u00b0C",
            "hvac_modes": ["off", "heat", "auto"],
            "presets": ["eco", "comfort"],
        },
    },
    {
        "key": "v1",
        "kind": "cover",
        "name": "Window blind",
        "features": {"open": True, "close": True, "stop": True, "position": True},
    },
    {"key": "s1", "kind": "scene", "name": "Film night"},
    {"key": "w1", "kind": "switch", "name": "Fan"},
    {"key": "t1", "kind": "sensor", "name": "CO2", "features": {"unit": "ppm"}},
]

DEMO_STATES = {
    "l1": {"on": True, "brightness": 60, "color_temp_k": 2700, "hs": None, "effect": None},
    "l2": {"on": False, "brightness": 0},
    "c1": {"hvac_mode": "heat", "current": 20.5, "target": 21, "preset": "comfort"},
    "v1": {"state": "open", "position": 40},
    "s1": {},
    "w1": {"on": False},
    "t1": {"value": "612"},
}


class DemoRoom:
    """Plays Home Assistant for a made-up room: carries out intents on its own
    states and reports the changes, as the integration will."""

    def __init__(self) -> None:
        self.states = json.loads(json.dumps(DEMO_STATES))
        self.intents: list[dict] = []
        self.fail_actions: set[str] = set()  # actions to answer with an error, for tests
        self.config: dict | None = None  # the panel's settings (section 12)
        self.display_states: list[dict] = []  # what the panel reported (section 13)
        self.log = False

    def apply(self, intent: dict) -> dict:
        """Carries out an intent; returns the item's changed fields."""
        key, action = intent["key"], intent["action"]
        args = intent.get("args", {})
        s = self.states[key]
        before = dict(s)
        if action == "turn_on":
            s["on"] = True
            s.update(args)
            if not s.get("brightness"):
                s["brightness"] = 100
        elif action == "turn_off":
            s["on"] = False
        elif action == "toggle":
            s["on"] = not s.get("on")
        elif action == "set_target":
            s["target"] = args["value"]
        elif action == "set_range":
            s["target_low"], s["target_high"] = args["low"], args["high"]
        elif action == "set_hvac_mode":
            s["hvac_mode"] = args["mode"]
        elif action == "set_preset":
            s["preset"] = args["preset"]
        elif action == "set_fan_mode":
            s["fan_mode"] = args["fan_mode"]
        elif action in ("open", "close"):
            s["state"] = "open" if action == "open" else "closed"
            s["position"] = 100 if action == "open" else 0
        elif action == "set_position":
            s["position"] = args["position"]
            s["state"] = "open" if args["position"] > 0 else "closed"
        elif action == "set_tilt":
            s["tilt"] = args["tilt"]
        return {k: v for k, v in s.items() if before.get(k) != v}

    async def serve(self, client: NestoClient, title: str = DEMO_TITLE) -> None:
        """Synchronises the room, then answers intents until the connection ends."""
        await client.synchronise(title, DEMO_ITEMS, self.states, self.config)

        async def keep_alive() -> None:
            while True:
                await asyncio.sleep(20)
                await client.send({"t": "ping", "seq": 0})

        pinger = asyncio.create_task(keep_alive())
        try:
            while True:
                message = await client.receive()
                if message.get("t") == "display_state":
                    self.display_states.append(message)
                    if self.log:
                        print(
                            f"display: {message['state']} at {message['brightness']}%"
                            f" ({message['cause']})",
                            flush=True,
                        )
                    continue
                if message.get("t") != "intent":
                    continue
                self.intents.append(message)
                if self.log:
                    args = json.dumps(message.get("args", {}))
                    print(f"{message['key']}: {message['action']} {args}", flush=True)
                if message["action"] in self.fail_actions:
                    error = {"code": "service_failed", "message": "the demo refused"}
                    await client.send(
                        {"t": "result", "re": message["id"], "ok": False, "error": error}
                    )
                    continue
                changes = self.apply(message)
                await client.send({"t": "result", "re": message["id"], "ok": True})
                if changes:
                    await client.send({"t": "states", "items": {message["key"]: changes}})
        finally:
            pinger.cancel()


# Command line ---------------------------------------------------------------


async def _pair(args: argparse.Namespace) -> None:
    client = await NestoClient.connect(args.host, args.port)
    print(f"Panel {client.hello['panel_id']}, firmware {client.hello['fw']}")

    async def ask(code: str) -> bool:
        if args.yes:
            print(f"Code: {code[:3]} {code[3:]}")
            return True
        entered = await asyncio.to_thread(input, "Code shown on the panel: ")
        return entered.replace(" ", "") == code

    try:
        key = await client.pair(ask)
    finally:
        await client.close()
    Path(args.key_file).write_bytes(key)
    os.chmod(args.key_file, 0o600)
    print(f"Paired. Key saved to {args.key_file}")


def read_key(path: str) -> bytes:
    try:
        key = Path(path).read_bytes()
    except FileNotFoundError:
        raise PanelError(f"no key in {path}: pair first") from None
    if len(key) != 32:
        raise PanelError(f"{path} doesn't hold a pairing key: pair again")
    return key


async def _connect(args: argparse.Namespace) -> None:
    key = read_key(args.key_file)
    client = await NestoClient.connect(args.host, args.port)
    await client.authenticate(key)
    await client.synchronise()
    info = await client.request({"t": "info"})
    print(f"Connected: {json.dumps(info)}")
    try:
        seq = 0
        while True:
            seq += 1
            await client.send({"t": "ping", "seq": seq})
            deadline = asyncio.get_running_loop().time() + 20
            while (remaining := deadline - asyncio.get_running_loop().time()) > 0:
                try:
                    message = await asyncio.wait_for(client.receive(), remaining)
                except TimeoutError:
                    break
                if message.get("t") != "pong":
                    print(json.dumps(message))
    finally:
        await client.close()


async def _demo(args: argparse.Namespace) -> None:
    key = read_key(args.key_file)
    client = await NestoClient.connect(args.host, args.port)
    await client.authenticate(key)
    room = DemoRoom()
    room.log = True
    # Sent every time, as Home Assistant does, so a setting tried once doesn't
    # stay on the panel.
    room.config = {"display": {"timeout_s": args.timeout, "wake_on_motion": not args.no_motion}}
    print(f"Serving the demo room '{DEMO_TITLE}' to panel {client.hello['panel_id']}")
    try:
        await room.serve(client)
    finally:
        await client.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--key-file", default="nesto.key")
    commands = parser.add_subparsers(dest="command", required=True)
    pair = commands.add_parser("pair", help="pair and save the key")
    pair.add_argument("--yes", action="store_true", help="accept the code without asking")
    commands.add_parser("connect", help="authenticate, synchronise and print messages")
    demo = commands.add_parser("demo", help="serve a made-up room that answers the panel's intents")
    demo.add_argument(
        "--timeout",
        type=int,
        default=30,
        help="seconds without input before the display rests (default: 30)",
    )
    demo.add_argument(
        "--no-motion",
        action="store_true",
        help="don't wake the display when someone comes near",
    )
    args = parser.parse_args()
    run = {"pair": _pair, "connect": _connect, "demo": _demo}[args.command]
    try:
        asyncio.run(run(args))
    except asyncio.IncompleteReadError:
        sys.exit("The panel closed the connection")
    except (PanelError, ConnectionError) as error:
        sys.exit(f"Error: {error}")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
