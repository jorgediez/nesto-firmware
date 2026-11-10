"""End-to-end: the Settings menu on the real panel, driven by its input script.

With no layout synchronised, the panel shows its waiting screen, where a press
opens Settings; the scripts start after the test has paired once.
"""

from __future__ import annotations

import asyncio
from pathlib import Path

import pytest
from conftest import Panel
from nesto_client import NestoClient, PanelError
from test_panel import pair

SETTLE_MS = 4000


def start(tmp_path: Path, steps: str) -> Panel:
    panel = Panel(tmp_path / "state", ["--no-mdns", "--script", f"w{SETTLE_MS} {steps}"])
    panel.start()
    return panel


def test_pairing_can_be_opened_again_from_settings(tmp_path: Path) -> None:
    # Waiting screen, press: Settings; press: Pair with Home Assistant.
    panel = start(tmp_path, "p p w500")
    try:

        async def run() -> None:
            old_key = await pair(panel)

            # Until the user opens pairing on the panel, a paired panel refuses.
            client = await NestoClient.connect("127.0.0.1", panel.port)
            assert client.hello["pairing_open"] is False
            await client.close()

            await asyncio.to_thread(panel.wait_for_log, "pairing opened", 15)
            client = await NestoClient.connect("127.0.0.1", panel.port)
            assert client.hello["paired"] is True
            assert client.hello["pairing_open"] is True
            await client.close()

            panel.lines.clear()  # so pair() sees the new code, not the first one
            new_key = await pair(panel)
            assert new_key != old_key

            # The old key no longer works; the new one does.
            client = await NestoClient.connect("127.0.0.1", panel.port)
            with pytest.raises(PanelError):
                await client.authenticate(old_key)
            await client.close()
            client = await NestoClient.connect("127.0.0.1", panel.port)
            await client.authenticate(new_key)
            await client.close()

        asyncio.run(run())
    finally:
        panel.stop()


def test_forget_home_assistant_from_settings(tmp_path: Path) -> None:
    # Settings, turn to Forget, press, and confirm with another press.
    panel = start(tmp_path, "p r+1 p p w500")
    try:

        async def run() -> None:
            await pair(panel)
            assert (panel.state_dir / "pairing.key").exists()
            await asyncio.to_thread(panel.wait_for_log, "forgot Home Assistant", 15)
            client = await NestoClient.connect("127.0.0.1", panel.port)
            assert client.hello["paired"] is False
            assert client.hello["pairing_open"] is True
            await client.close()

        asyncio.run(run())
        assert not (panel.state_dir / "pairing.key").exists()
    finally:
        panel.stop()


def test_holding_cancels_forgetting(tmp_path: Path) -> None:
    # Settings, Forget, then hold instead of confirming.
    panel = start(tmp_path, "p r+1 p h w500")
    try:

        async def run() -> None:
            await pair(panel)
            await asyncio.sleep(SETTLE_MS / 1000 + 2.5)
            client = await NestoClient.connect("127.0.0.1", panel.port)
            assert client.hello["paired"] is True
            await client.close()

        asyncio.run(run())
        assert not any("forgot Home Assistant" in line for line in panel.lines)
    finally:
        panel.stop()
