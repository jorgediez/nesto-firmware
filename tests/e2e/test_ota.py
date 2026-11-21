"""End-to-end: a firmware update from "Home Assistant" (PROTOCOL.md section 19).

The simulator trusts the test key in tests/ota/, whose private half signs
here as a release key would. The update goes to a stand-in program file
(--ota-target), as the boot script points Nesto at /nesto/nesto.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import os
from pathlib import Path

import pytest
from conftest import Panel
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from nesto_client import NestoClient, PanelError
from ota_sign import ota_signed_text
from test_panel import pair

ROOT = Path(__file__).resolve().parents[2]
TEST_KEY = Ed25519PrivateKey.from_private_bytes(
    bytes.fromhex((ROOT / "tests/ota/test-key").read_text().strip())
)
OLD = b"the program running now"


def sign(program: bytes, version: str, key: Ed25519PrivateKey = TEST_KEY) -> str:
    text = ota_signed_text("nest-gen2", version, len(program), hashlib.sha512(program).digest())
    return base64.b64encode(key.sign(text)).decode()


def ota_panel(tmp_path: Path) -> tuple[Panel, Path]:
    target = tmp_path / "nesto"
    target.write_bytes(OLD)
    panel = Panel(tmp_path / "state", ["--no-mdns", "--ota-target", str(target)])
    panel.start()
    return panel, target


async def connected(panel: Panel, key: bytes) -> NestoClient:
    client = await NestoClient.connect("127.0.0.1", panel.port)
    await client.authenticate(key)
    await client.synchronise()
    return client


def test_a_signed_update_goes_in_place_and_is_confirmed_after_a_restart(tmp_path: Path) -> None:
    panel, target = ota_panel(tmp_path)
    program = os.urandom(200 * 1024 + 123)  # four chunks, the last one short
    state = tmp_path / "state"

    async def update() -> bytes:
        key = await pair(panel)
        client = await connected(panel, key)
        assert client.hello["caps"]["ota"] is True
        sent: list[int] = []
        await client.update(
            program, "9.0.0", sign(program, "9.0.0"), progress=lambda done, total: sent.append(done)
        )
        assert sent == [65536, 131072, 196608, len(program)]
        await client.close()
        return key

    try:
        key = asyncio.run(update())
        # The panel exits for the boot script to start the new program.
        assert panel.wait_exit(10) == 4
        assert target.read_bytes() == program
        assert (tmp_path / "nesto.prev").read_bytes() == OLD
        assert not (tmp_path / "nesto.new").exists()
        assert (state / "ota-pending").read_text() == "9.0.0\n"
        panel.wait_for_log(r"update to Nesto 9\.0\.0: in place")

        # Started again: once Home Assistant is back, it confirms the update.
        panel.start()

        async def back() -> None:
            client = await connected(panel, key)  # synchronised: the panel is ready
            await client.close()

        asyncio.run(back())
        panel.wait_for_log(r"update to Nesto 9\.0\.0 confirmed")
        assert not (state / "ota-pending").exists()
    finally:
        panel.stop()


def test_what_it_refuses(tmp_path: Path) -> None:
    panel, target = ota_panel(tmp_path)
    program = b"a program"

    async def refusals() -> None:
        key = await pair(panel)
        client = await connected(panel, key)

        async def code(message: dict) -> str | None:
            with pytest.raises(PanelError) as error:
                await client.request(message)
            return error.value.code

        begin = {
            "t": "ota_begin",
            "model": "nest-gen2",
            "version": "9.0.0",
            "size": len(program),
            "sha512": hashlib.sha512(program).hexdigest(),
        }
        stranger = Ed25519PrivateKey.generate()
        assert (
            await code({**begin, "signature": sign(program, "9.0.0", stranger)}) == "bad_signature"
        )
        assert await code({**begin, "version": "0.0.1", "signature": sign(program, "0.0.1")}) == (
            "downgrade"
        )
        assert await code({**begin, "model": "nest-gen3", "signature": sign(program, "9.0.0")}) == (
            "unsupported"
        )
        assert await code({"t": "ota_apply"}) == "bad_request"  # nothing received
        # The same program with one byte changed: its SHA-512 tells at the end.
        await client.request({**begin, "signature": sign(program, "9.0.0")})
        changed = b"A" + program[1:]
        await client.request(
            {"t": "ota_chunk", "offset": 0, "data": base64.b64encode(changed).decode()}
        )
        assert await code({"t": "ota_end"}) == "bad_signature"
        await client.close()

    try:
        asyncio.run(refusals())
        assert target.read_bytes() == OLD
        assert not (tmp_path / "nesto.new").exists()
    finally:
        panel.stop()


def test_without_a_target_it_takes_no_updates(panel: Panel) -> None:
    async def session() -> None:
        key = await pair(panel)
        client = await connected(panel, key)
        assert client.hello["caps"]["ota"] is False
        with pytest.raises(PanelError) as error:
            await client.request({"t": "ota_end"})
        assert error.value.code == "unsupported"
        await client.close()

    asyncio.run(session())
