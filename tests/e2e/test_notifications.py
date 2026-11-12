"""End-to-end: notifications (PROTOCOL.md section 14).

The demo room sends `notify` requests a set time after synchronising; the
panel pops them up, wakes the display as their priority says, and reports
what became of each with `notify_event`.
"""

from __future__ import annotations

import asyncio
from pathlib import Path

from conftest import Panel
from nesto_client import DemoRoom, NestoClient
from test_panel import pair
from test_room import SETTLE_MS


def serve(tmp_path: Path, notices: list, steps: str, seconds: float, config: dict | None = None):
    """Serves the demo room to a fresh panel, sending `notices` (seconds after
    synchronising, message) and playing `steps` after the settling time."""
    panel = Panel(tmp_path / "state", ["--no-mdns", "--script", f"w{SETTLE_MS} {steps}"])
    panel.start()
    room = DemoRoom()
    room.config = config
    room.notices = notices

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


def events(room: DemoRoom) -> list[str]:
    return [
        f"{e['nid']} {e['event']}" + (f" {e['action']}" if "action" in e else "")
        for e in room.notify_events
    ]


def causes(room: DemoRoom) -> list[tuple[str, str]]:
    return [(s["state"], s["cause"]) for s in room.display_states]


REST = {"display": {"timeout_s": 2}}


def test_a_normal_one_wakes_standby_and_expires(tmp_path: Path) -> None:
    notice = {
        "nid": "door",
        "title": "Front door",
        "message": "Someone is at the door",
        "duration_s": 2,
    }
    room, _ = serve(tmp_path, [(5, notice)], "w100", seconds=10, config=REST)
    assert [r["ok"] for r in room.notify_results] == [True]
    assert ("active", "notification") in causes(room)
    assert events(room) == ["door shown", "door expired"]


def test_a_low_one_waits_in_the_list(tmp_path: Path) -> None:
    notice = {"nid": "bins", "message": "Bins go out tonight", "priority": "low"}
    room, _ = serve(tmp_path, [(5, notice)], "w100", seconds=8, config=REST)
    assert ("active", "notification") not in causes(room)
    assert events(room) == []


def test_a_high_one_wakes_off(tmp_path: Path) -> None:
    config = {"display": {"timeout_s": 2}, "standby": {"mode": "off"}}
    normal = {"nid": "a", "message": "Not now", "duration_s": 1}
    high = {"nid": "b", "message": "Now", "priority": "high", "duration_s": 1}
    room, _ = serve(tmp_path, [(5, normal), (6, high)], "w100", seconds=9, config=config)
    assert causes(room).count(("active", "notification")) == 1
    assert events(room) == ["b shown", "b expired"]


def test_the_ring_chooses_an_action(tmp_path: Path) -> None:
    notice = {
        "nid": "door",
        "message": "Someone is at the door",
        "duration_s": 0,
        "actions": [{"id": "open", "label": "Open"}, {"id": "ignore", "label": "Ignore"}],
    }
    room, _ = serve(tmp_path, [(0.5, notice)], "r+1 p w500", seconds=7)
    assert events(room) == ["door shown", "door action ignore"]
    assert room.intents == []  # the press was the notification's


def test_clearing_and_bad_requests(tmp_path: Path) -> None:
    notice = {"nid": "door", "message": "Someone is at the door", "duration_s": 0}
    clear = {"t": "notify_clear", "nid": "door"}
    bad = {"nid": "x", "message": "", "priority": "urgent"}
    # Cleared before the long press, which then switches the light again.
    room, _ = serve(tmp_path, [(0.5, notice), (1.5, clear), (2, bad)], "h w500", seconds=7)
    assert events(room) == ["door shown"]
    assert [r["ok"] for r in room.notify_results] == [True, False]
    assert room.notify_results[1]["error"]["code"] == "bad_request"
    assert len(room.intents) == 1
