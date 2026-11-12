"""End-to-end: display power and the standby clock (PROTOCOL.md sections 12,
13 and 16).

The panel rests after the timeout Home Assistant sets, on the clock once it
knows the time, wakes on input or motion without passing the waking input on,
answers the `display` request, reports every change, and keeps its settings
and UTC offset across a restart.
"""

from __future__ import annotations

import asyncio
import json
import re
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
    room, lines = serve(tmp_path, "w3500 h w300 h w500", seconds=11, config=QUICK)
    assert changes(room)[:3] == [
        ("active", 80, "sync"),
        ("standby", 20, "timeout"),  # the time is known: the clock shows
        ("active", 80, "input"),
    ]
    assert [(i["key"], i["action"]) for i in room.intents] == [("l1", "turn_off")]
    assert any("standby clock shown, analog" in line for line in lines)
    assert any("time from Home Assistant" in line for line in lines)


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


def test_the_digital_clock(tmp_path: Path) -> None:
    config = {
        "display": {"timeout_s": 2},
        "standby": {"brightness": 35, "clock": {"style": "digital", "seconds": True}},
        "locale": {"hour12": True},
    }
    room, lines = serve(tmp_path, "w4000", seconds=7, config=config)
    assert ("standby", 35, "timeout") in changes(room)
    assert any(
        re.search(r"standby clock shown, digital: \d{1,2}:\d\d:\d\d [AP]M", line) for line in lines
    )


def test_without_the_time_standby_is_dark(tmp_path: Path) -> None:
    state = tmp_path / "state"
    state.mkdir()
    (state / "display.json").write_text(json.dumps(QUICK))
    panel = Panel(state, ["--no-mdns"])
    panel.start()
    try:
        panel.wait_for_log(r"no stored UTC offset", timeout=5)
        panel.wait_for_log(r"display standby at 0% \(timeout\)", timeout=10)
        assert not any("standby clock shown" in line for line in panel.lines)
    finally:
        panel.stop()


def test_the_settings_and_offset_survive_a_restart(tmp_path: Path) -> None:
    serve(tmp_path, "w100", seconds=6, config=QUICK)
    stored = json.loads((tmp_path / "state" / "display.json").read_text())
    assert stored["display"]["timeout_s"] == 2
    offset = json.loads((tmp_path / "state" / "time.json").read_text())
    assert isinstance(offset["utc_offset_min"], int)

    # Without Home Assistant, the restarted panel rests on the stored timeout,
    # on the clock: the system clock and the stored offset give the time.
    panel = Panel(tmp_path / "state", ["--no-mdns"])
    panel.start()
    try:
        panel.wait_for_log(r"time from the system clock", timeout=5)
        panel.wait_for_log(r"display standby at 20% \(timeout\)", timeout=10)
        panel.wait_for_log(r"standby clock shown", timeout=5)
    finally:
        panel.stop()


def light(raw: int, seconds: int) -> str:
    """The light sensor reading `raw` once a second for `seconds`, as the
    thermostat's base sends it."""
    return " ".join(f"l{raw} w1000" for _ in range(seconds))


def test_a_dark_room_dims_the_controls_and_the_clock(tmp_path: Path) -> None:
    # Readings start a little after Home Assistant connects; the timeout
    # comes after them.
    config = {"display": {"timeout_s": 6}}
    room, lines = serve(tmp_path, light(100, 9), seconds=12, config=config)
    assert any("auto brightness: 20% of 80% (light 100)" in line for line in lines)
    # Standby is 20%, so a quarter would be less than the 5% minimum.
    assert ("standby", 5, "timeout") in changes(room)
    assert any(
        "display standby at 20%, 5% for the room's light (timeout)" in line for line in lines
    )


def test_switching_the_lights_on_brightens_it_again(tmp_path: Path) -> None:
    config = {"display": {"timeout_s": 0}}
    _, lines = serve(tmp_path, f"{light(100, 3)} {light(2000, 7)}", seconds=13, config=config)
    glides = [line for line in lines if "auto brightness:" in line]
    assert "auto brightness: 20% of 80%" in glides[0]
    assert "auto brightness: 80% of 80%" in glides[-1]


def test_without_auto_brightness_the_light_changes_nothing(tmp_path: Path) -> None:
    config = {"display": {"timeout_s": 2, "auto_brightness": False}}
    room, lines = serve(tmp_path, light(100, 6), seconds=9, config=config)
    assert ("standby", 20, "timeout") in changes(room)
    assert not any("auto brightness:" in line or "room's light" in line for line in lines)


def test_waking_goes_back_to_the_top_of_the_home_screen(tmp_path: Path) -> None:
    # On the third item when the display rests; woken, a long press acts on
    # the first.
    room, _ = serve(tmp_path, "r+2 w3500 p w300 h w500", seconds=11, config=QUICK)
    assert [(i["key"], i["action"]) for i in room.intents] == [("l1", "turn_off")]
