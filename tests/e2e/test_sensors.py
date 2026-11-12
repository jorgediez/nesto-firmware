"""End-to-end: the base's sensors and press gestures for Home Assistant
(PROTOCOL.md section 17).

The simulator's --fake-base stands in for the thermostat's base: fixed
readings, the light from the script's lN, and motion from m (near) and f (far).
"""

from __future__ import annotations

import asyncio
from pathlib import Path

from conftest import Panel
from nesto_client import DemoRoom, NestoClient
from test_panel import pair
from test_room import SETTLE_MS

BASE_SENSORS = [
    "temperature",
    "humidity",
    "illuminance",
    "motion_near",
    "motion_far",
    "supply_voltage",
    "usb_voltage",
    "battery_voltage",
]


def serve(tmp_path: Path, steps: str, seconds: float, args: list[str], config: dict | None = None):
    """Runs `steps` after the settling time on a fresh panel started with
    `args`, serving the demo room; returns the hello, the room and the info
    result taken before the room was served."""
    panel = Panel(tmp_path / "state", ["--no-mdns", *args, "--script", f"w{SETTLE_MS} {steps}"])
    panel.start()
    room = DemoRoom()
    room.config = config
    out: dict = {}

    async def session() -> None:
        key = await pair(panel)
        client = await NestoClient.connect("127.0.0.1", panel.port)
        await client.authenticate(key)
        out["hello"] = client.hello
        serving = asyncio.create_task(room.serve(client))
        await asyncio.sleep(seconds)
        serving.cancel()
        await client.close()

        # A second connection asks for the diagnostics.
        client = await NestoClient.connect("127.0.0.1", panel.port)
        await client.authenticate(key)
        await client.synchronise()
        out["info"] = await client.request({"t": "info"})
        await client.close()

    try:
        asyncio.run(session())
    finally:
        panel.stop()
    return out["hello"], room, out["info"]


def test_the_base_s_readings_and_motion(tmp_path: Path) -> None:
    # Someone near, then moving in the room after the near one has ended.
    hello, room, info = serve(tmp_path, "l1950 m w7000 f w1000", seconds=13, args=["--fake-base"])
    assert hello["caps"]["sensors"] == BASE_SENSORS
    assert info["base_connected"] is True
    assert info["base_fw"] == "simulated"

    first = room.sensors[0]
    assert first["temperature_c"] == 21.5
    assert first["humidity_pct"] == 45.0
    assert first["supply_v"] == 0.0  # no heating wires: on a stand
    assert first["usb_v"] == 4.94
    assert first["battery_v"] == 3.91
    assert first["motion_near"] is False

    # Each change of motion at once, well within the 30 s interval.
    motion = [(s["motion_near"], s["motion_far"]) for s in room.sensors]
    assert motion[:4] == [(False, False), (True, False), (False, False), (False, True)]
    assert room.sensors[1]["illuminance_raw"] == 1950


def test_press_gestures_are_reported(tmp_path: Path) -> None:
    # A press on the controls, before the timeout (counted from the config,
    # then from the press); after it, a double press, which doesn't wake the
    # display, then a long press, which does.
    config = {"display": {"timeout_s": 5}}
    _, room, _ = serve(
        tmp_path, "p w6000 p p w1500 h w500", seconds=13, args=["--fake-base"], config=config
    )
    assert [(i["gesture"], i["context"]) for i in room.inputs] == [
        ("single", "ui"),
        ("double", "standby"),
        ("long", "standby"),
    ]
    states = [(s["state"], s["cause"]) for s in room.display_states]
    assert states[1:] == [("standby", "timeout"), ("active", "input")]


def test_without_a_base_there_are_no_sensors(tmp_path: Path) -> None:
    hello, room, info = serve(tmp_path, "m w500", seconds=4, args=[])
    assert hello["caps"]["sensors"] == []
    assert room.sensors == []
    assert info["base_connected"] is False
    assert info["base_fw"] is None
