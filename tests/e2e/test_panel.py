"""End-to-end: the real panel program against the Python client, over TCP.

These check what the unit tests can't: sockets, the main loop, the key file on
disk, restarts, and that C++ and Python agree on the wire.
"""

from __future__ import annotations

import asyncio

import pytest
from conftest import PANEL_ID, VERSION, Panel
from nesto_client import NestoClient, PanelError


async def connect(panel: Panel) -> NestoClient:
    return await NestoClient.connect("127.0.0.1", panel.port)


async def pair(panel: Panel) -> bytes:
    """Pairs, checking the panel showed the same code the client computed."""
    client = await connect(panel)

    async def confirm(code: str) -> bool:
        shown = await asyncio.to_thread(panel.wait_for_log, r"pairing code (\d{6})")
        assert shown.group(1) == code
        return True

    key = await client.pair(confirm)
    await client.close()
    return key


async def session(panel: Panel, key: bytes) -> NestoClient:
    client = await connect(panel)
    await client.authenticate(key)
    await client.synchronise()
    return client


def test_hello_describes_an_unpaired_panel(panel: Panel) -> None:
    async def run() -> None:
        client = await connect(panel)
        hello = client.hello
        assert hello["proto"] == 1
        assert hello["panel_id"] == PANEL_ID
        assert hello["paired"] is False
        assert hello["pairing_open"] is True
        assert hello["caps"]["display"] == {"width": 320, "height": 320, "shape": "round"}
        assert "fan" in hello["caps"]["kinds"]
        await client.close()

    asyncio.run(run())


def test_pair_authenticate_and_talk(panel: Panel) -> None:
    async def run() -> None:
        key = await pair(panel)
        panel.wait_for_log(r"paired with Home Assistant")

        client = await session(panel, key)
        assert client.hello["paired"] is True
        assert client.hello["pairing_open"] is False

        await client.send({"t": "ping", "seq": 42})
        assert (await client.wait_for("pong"))["seq"] == 42

        info = await client.request({"t": "info"})
        assert info["fw"] == VERSION
        assert info["free_ram_kb"] > 0
        # No Wi-Fi in the container: the fields are there, and null.
        assert info["wifi_rssi"] is None
        assert info["wifi_ssid"] is None
        assert info["wifi_freq_mhz"] is None

        # Home Assistant's versions, logged as they arrive.
        await client.send(
            {"t": "ha_info", "ha_version": "2026.11.1", "integration_version": "0.1.0"}
        )
        panel.wait_for_log(r"Home Assistant 2026\.11\.1, ha-nesto 0\.1\.0")

        await client.request({"t": "identify"})

        with pytest.raises(PanelError) as error:
            await client.request({"t": "teleport"})
        assert error.value.code == "unknown_type"
        await client.close()

    asyncio.run(run())


def test_a_wrong_code_leaves_the_panel_unpaired(panel: Panel) -> None:
    async def run() -> None:
        client = await connect(panel)

        async def mismatch(code: str) -> bool:
            return False

        with pytest.raises(PanelError):
            await client.pair(mismatch)
        await client.close()

        again = await connect(panel)
        assert again.hello["paired"] is False
        await again.close()

    asyncio.run(run())


def test_a_wrong_key_is_refused(panel: Panel) -> None:
    async def run() -> None:
        await pair(panel)
        client = await connect(panel)
        with pytest.raises(PanelError) as error:
            await client.authenticate(bytes(32))
        assert error.value.code == "auth_failed"
        await client.close()

    asyncio.run(run())


def test_the_pairing_survives_a_restart(panel: Panel) -> None:
    async def run() -> None:
        key = await pair(panel)
        panel.restart()
        client = await session(panel, key)
        assert client.hello["paired"] is True
        await client.close()

    asyncio.run(run())
    assert (panel.state_dir / "pairing.key").stat().st_mode & 0o777 == 0o600


def test_a_newer_session_replaces_the_older_one(panel: Panel) -> None:
    async def run() -> None:
        key = await pair(panel)
        first = await session(panel, key)
        await first.send({"t": "ping", "seq": 1})
        assert (await first.wait_for("pong"))["seq"] == 1

        second = await session(panel, key)
        # The panel closes the first connection, and says why.
        with pytest.raises((asyncio.IncompleteReadError, ConnectionError)):
            await asyncio.wait_for(first.receive(), 5)
        panel.wait_for_log(r"replaced by a newer connection")
        await second.send({"t": "ping", "seq": 1})
        assert (await second.wait_for("pong"))["seq"] == 1
        await first.close()
        await second.close()

    asyncio.run(run())


def test_unpair_forgets_the_key(panel: Panel) -> None:
    async def run() -> None:
        key = await pair(panel)
        client = await session(panel, key)
        await client.request({"t": "unpair"})
        await client.close()

        again = await connect(panel)
        assert again.hello["paired"] is False
        await again.close()

    asyncio.run(run())
    assert not (panel.state_dir / "pairing.key").exists()


def test_a_bad_frame_closes_only_that_connection(panel: Panel) -> None:
    async def run() -> None:
        bad = await connect(panel)
        await bad.send_raw(b"\x00\x00\x00\x00")
        with pytest.raises((asyncio.IncompleteReadError, ConnectionError)):
            await asyncio.wait_for(bad.receive(), 5)
        await bad.close()

        good = await connect(panel)  # the panel is still up
        assert good.hello["panel_id"] == PANEL_ID
        await good.close()

    asyncio.run(run())
