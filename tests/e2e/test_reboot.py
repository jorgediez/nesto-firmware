"""End-to-end: Home Assistant reboots the panel (PROTOCOL.md section 18).

On the thermostat Nesto stops, hands the base back and reboots it; the
simulator stops the same way and exits with 5 (kRebootExit) instead.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
from pathlib import Path

import pytest
from conftest import Panel
from nesto_client import NestoClient, PanelError
from test_ota import connected, ota_panel, sign
from test_panel import pair

REBOOT_EXIT = 5


def test_a_reboot_is_answered_then_the_panel_stops(panel: Panel) -> None:
    async def reboot() -> None:
        key = await pair(panel)
        client = await connected(panel, key)
        assert client.hello["caps"]["reboot"] is True
        await client.request({"t": "reboot"})
        # Asked once already.
        with pytest.raises(PanelError) as error:
            await client.request({"t": "reboot"})
        assert error.value.code == "bad_request"
        await client.close()

    asyncio.run(reboot())
    assert panel.wait_exit(10) == REBOOT_EXIT
    panel.wait_for_log(r"Home Assistant asked for a reboot")
    panel.wait_for_log(r"the thermostat would reboot now")


def test_no_reboot_during_an_update(tmp_path: Path) -> None:
    panel, _ = ota_panel(tmp_path)
    program = b"a program"

    async def refused() -> None:
        key = await pair(panel)
        client = await connected(panel, key)
        await client.request(
            {
                "t": "ota_begin",
                "model": "nest-gen2",
                "version": "9.0.0",
                "size": len(program),
                "sha512": hashlib.sha512(program).hexdigest(),
                "signature": sign(program, "9.0.0"),
            }
        )
        await client.request(
            {"t": "ota_chunk", "offset": 0, "data": base64.b64encode(program[:4]).decode()}
        )
        with pytest.raises(PanelError) as error:
            await client.request({"t": "reboot"})
        assert error.value.code == "bad_request"
        assert "update" in str(error.value)
        await client.close()

    try:
        asyncio.run(refused())
        assert panel.wait_exit(2) is None  # still running
    finally:
        panel.stop()


def test_the_panel_says_it_can(panel: Panel) -> None:
    async def hello() -> None:
        client = await NestoClient.connect("127.0.0.1", panel.port)
        assert client.hello["caps"]["reboot"] is True
        await client.close()

    asyncio.run(hello())
