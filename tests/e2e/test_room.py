"""End-to-end: the ring and press on the real panel, against the demo room.

The panel plays scripted input (`--script`) after giving the test a few seconds
to pair and synchronise; the demo room plays Home Assistant, carrying out the
intents and reporting the new states. The tests check the intents that arrive.
"""

from __future__ import annotations

import asyncio
from pathlib import Path

from conftest import Panel
from nesto_client import DemoRoom, NestoClient
from test_panel import pair

# Time for pairing and synchronisation before the script's input starts,
# generous for the ARM build under QEMU.
SETTLE_MS = 4000


def run(tmp_path: Path, steps: str, seconds: float, fail: set[str] | None = None):
    """Runs `steps` on a fresh panel serving the demo room; returns the intents
    and the panel's log."""
    panel = Panel(tmp_path / "state", ["--no-mdns", "--script", f"w{SETTLE_MS} {steps}"])
    panel.start()
    room = DemoRoom()
    room.fail_actions = fail or set()

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
    return room.intents, panel.lines


def actions(intents: list[dict]) -> list[tuple]:
    return [(i["key"], i["action"], i.get("args", {})) for i in intents]


def test_brightness_follows_the_ring(tmp_path: Path) -> None:
    # Open the first light (60%) and turn three steps: 65% at once, 75% at the end.
    intents, _ = run(tmp_path, "p r+1 r+1 r+1 w800", seconds=7)
    sent = actions(intents)
    assert sent[0] == ("l1", "turn_on", {"brightness": 65})
    assert sent[-1] == ("l1", "turn_on", {"brightness": 75})
    assert len(sent) <= 3  # paced, not one per step


def test_climate_waits_for_the_ring_to_stop(tmp_path: Path) -> None:
    # Select the radiator, open it, raise the target twice: one intent, 1.5 s later.
    intents, _ = run(tmp_path, "r+2 p r+1 r+1 w2500", seconds=9)
    assert actions(intents) == [("c1", "set_target", {"value": 22.0})]


def test_switches_toggle_and_scenes_activate(tmp_path: Path) -> None:
    intents, _ = run(tmp_path, "r+5 p r-1 p w500", seconds=7)
    assert actions(intents) == [("w1", "turn_on", {}), ("s1", "activate", {})]


def test_a_fan_steps_through_its_speeds(tmp_path: Path) -> None:
    # Select the ceiling fan (off, four speeds), open it, one click up, then oscillate.
    intents, _ = run(tmp_path, "r+6 p r+1 w800 p p r+1 w500", seconds=9)
    assert actions(intents) == [
        ("f1", "set_percentage", {"percentage": 25}),
        ("f1", "oscillate", {"oscillating": True}),
    ]


def test_a_long_press_on_home_toggles_a_light(tmp_path: Path) -> None:
    intents, _ = run(tmp_path, "h w500", seconds=7)
    assert actions(intents) == [("l1", "turn_off", {})]


def test_a_failed_intent_is_reported(tmp_path: Path) -> None:
    _, log = run(tmp_path, "r+5 p w500", seconds=7, fail={"turn_on"})
    assert any("intent for w1 failed: the demo refused" in line for line in log)
