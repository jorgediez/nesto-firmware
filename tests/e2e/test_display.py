"""End-to-end: display power (PROTOCOL.md sections 12 and 13).

The panel rests after the timeout Home Assistant sets, wakes on input or
motion without passing the waking input on, answers the `display` request,
reports every change, and keeps its settings across a restart.
"""

from __future__ import annotations

import asyncio
import json
from pathlib import Path

import pytest
from conftest import Panel
from nesto_client import DemoRoom, NestoClient, PanelError
from test_panel import pair
from test_room import SETTLE_MS

QUICK = {"display": {"timeout_s": 2}}


def serve(tmp_path: Path, steps: str, seconds: float, config: dict | None = None):
    """Runs `steps` after the settling time on a fresh panel serving the demo
    room with `config`; returns the room and the panel's log."""
    panel = Panel(tmp_path / "state", ["--no-mdns", "--script", f"w{SETTLE_MS} {steps}"])
    panel.start()
    room = DemoRoom()
    room.config = config

    async def session() -> None:
        key = await pair(panel)
        client = await NestoClient.connect("127.0.0.1", panel.port)
        await client.authenticate(key)
        serving = asyncio.create_task(room.serve(client))
        await asyncio.sleep(seconds)
        serving.cancel()
        await client.close()

    try:
        asyncio.run(session())
    finally:
        panel.stop()
    return room, panel.lines


def changes(room: DemoRoom) -> list[tuple]:
    return [(s["state"], s["brightness"], s["cause"]) for s in room.display_states]


def test_the_timeout_rests_the_display_and_the_waking_press_only_wakes_it(tmp_path: Path) -> None:
    # Two long presses after the timeout: the first only wakes the display; the
    # second, on the home screen, switches the first light off.
    room, _ = serve(tmp_path, "w3500 h w300 h w500", seconds=11, config=QUICK)
    assert changes(room)[:3] == [
        ("active", 80, "sync"),
        ("standby", 0, "timeout"),  # dark until there's a clock to show
        ("active", 80, "input"),
    ]
    assert [(i["key"], i["action"]) for i in room.intents] == [("l1", "turn_off")]


def test_motion_wakes_it(tmp_path: Path) -> None:
    room, _ = serve(tmp_path, "w3500 m w500", seconds=9, config=QUICK)
    assert ("active", 80, "motion") in changes(room)
    assert room.intents == []


def test_standby_off_and_home_assistant_s_display_request(tmp_path: Path) -> None:
    config = {"display": {"timeout_s": 0, "brightness": 60}, "standby": {"mode": "off"}}
    panel = Panel(tmp_path / "state", ["--no-mdns"])
    panel.start()

    async def session() -> None:
        key = await pair(panel)
        client = await NestoClient.connect("127.0.0.1", panel.port)
        await client.authenticate(key)
        await client.synchronise(config=config)
        assert await client.wait_for("display_state") == {
            "t": "display_state",
            "state": "active",
            "brightness": 60,
            "cause": "sync",
        }
        await client.request({"t": "display", "state": "off"})
        off = await client.wait_for("display_state")
        assert (off["state"], off["brightness"], off["cause"]) == ("off", 0, "ha")
        await client.request({"t": "display", "state": "active"})
        on = await client.wait_for("display_state")
        assert (on["state"], on["brightness"], on["cause"]) == ("active", 60, "ha")
        with pytest.raises(PanelError) as refused:
            await client.request({"t": "display", "state": "asleep"})
        assert refused.value.code == "bad_request"
        await client.close()

    try:
        asyncio.run(session())
    finally:
        panel.stop()


def test_the_settings_survive_a_restart(tmp_path: Path) -> None:
    serve(tmp_path, "w100", seconds=6, config=QUICK)
    stored = json.loads((tmp_path / "state" / "display.json").read_text())
    assert stored["display"]["timeout_s"] == 2

    # Without Home Assistant, the restarted panel rests on the stored timeout.
    panel = Panel(tmp_path / "state", ["--no-mdns"])
    panel.start()
    try:
        panel.wait_for_log(r"display standby at 0% \(timeout\)", timeout=10)
    finally:
        panel.stop()
