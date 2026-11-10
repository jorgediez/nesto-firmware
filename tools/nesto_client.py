"""A small Home Assistant stand-in that talks to a Nesto panel.

It does what the ha-nesto integration will do: pair, authenticate, send the
synchronisation messages and exchange messages over the encrypted session. The
end-to-end tests use it, and it is handy by hand against the simulator:

    python3 tools/nesto_client.py 127.0.0.1 pair
    python3 tools/nesto_client.py 127.0.0.1 connect

`pair` asks for the code the panel shows and saves the key to a file
(nesto.key by default); `connect` uses that key, synchronises, and prints what
the panel sends until interrupted.
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

    async def synchronise(self, title: str = "Test room") -> None:
        """Sends the four synchronisation messages (section 8) and waits for ready."""
        now = datetime.now().astimezone()
        offset = now.utcoffset() or timedelta()
        await self.send(
            {
                "t": "time",
                "utc_ms": int(now.timestamp() * 1000),
                "utc_offset_min": int(offset.total_seconds() // 60),
            }
        )
        await self.send({"t": "config"})
        await self.send({"t": "layout", "rev": 1, "title": title, "items": []})
        await self.send({"t": "states", "full": True, "items": {}})
        await self.wait_for("ready")


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


async def _connect(args: argparse.Namespace) -> None:
    key = Path(args.key_file).read_bytes()
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--key-file", default="nesto.key")
    commands = parser.add_subparsers(dest="command", required=True)
    pair = commands.add_parser("pair", help="pair and save the key")
    pair.add_argument("--yes", action="store_true", help="accept the code without asking")
    commands.add_parser("connect", help="authenticate, synchronise and print messages")
    args = parser.parse_args()
    try:
        asyncio.run(_pair(args) if args.command == "pair" else _connect(args))
    except (PanelError, ConnectionError, asyncio.IncompleteReadError) as error:
        sys.exit(f"Error: {error}")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
