"""Screenshots of the panel's screens, with the demo room playing Home Assistant.

    python3 tools/screens.py OUTDIR [--panel CMD] [--only NAME ...]

For each screen, a headless panel starts with an input script and saves its
screen when the script ends; meanwhile this pairs with it and serves the demo
room. CI keeps the results, so a pull request shows what the screens look like.
"""

from __future__ import annotations

import argparse
import asyncio
import shlex
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

from nesto_client import DemoRoom, NestoClient

# Pairing and synchronisation happen in the first seconds; input starts after.
SETTLE = "w4000"

SCREENS = {
    "waiting": None,  # no Home Assistant at all
    "home": f"{SETTLE} w200",
    "home-fan": f"{SETTLE} r+5 w300",
    "light": f"{SETTLE} p r+2 w2500",
    "light-colour-temperature": f"{SETTLE} p p r-3 w2500",
    "climate": f"{SETTLE} r+2 p r+2 w2500",
    "climate-mode": f"{SETTLE} r+2 p p w300",
    "cover": f"{SETTLE} r+3 p r-2 w2500",
    "settings": f"{SETTLE} r+7 p w300",
    "settings-forget": f"{SETTLE} r+7 p r+1 p w300",
    "settings-info": f"{SETTLE} r+7 p r+3 p w500",
    "settings-info-ha": f"{SETTLE} r+7 p r+3 p w3500 r+7 w300",
    "settings-info-network": f"{SETTLE} r+7 p r+3 p w500 r+12 w300",
    # Wi-Fi, with the simulator's pretend networks: the list once scanned,
    # typing a password on the ring (letters, then the digits page), and
    # joining an open network.
    "wifi": f"{SETTLE} r+7 p r+2 p w2000",
    "wifi-keyboard": f"{SETTLE} r+7 p r+2 p w2000 r+2 p r+15 p r+3 p r-11 p r+4 w300",
    "wifi-keyboard-digits": f"{SETTLE} r+7 p r+2 p w2000 r+2 p r-3 p r+4 p w300",
    "wifi-joining": f"{SETTLE} r+7 p r+2 p w2000 r+3 p w300",
    "wifi-forget": f"{SETTLE} r+7 p r+2 p w2000 p w300",
    "accents": f"{SETTLE} r+1 w300",
    # The timeout below rests the display; the clock shows once the fade ends.
    "standby-analog": f"{SETTLE} w3500",
    "standby-digital": f"{SETTLE} w3500",
    "standby-digital-12h": f"{SETTLE} w3500",
    # Notifications, sent right after synchronising (NOTICES below).
    "notice": f"{SETTLE} r+1 w300",
    "notice-critical": f"{SETTLE} w300",
    # The selection stays on the light it was on; the entry is just above.
    "home-notifications": f"{SETTLE} r-1 w300",
    "notifications": f"{SETTLE} r-1 p r+1 w300",
}

# Settings for the screens that need some, as Home Assistant sends them.
CONFIGS = {
    "standby-analog": {"display": {"timeout_s": 2}, "standby": {"clock": {"seconds": True}}},
    "standby-digital": {
        "display": {"timeout_s": 2},
        "standby": {"clock": {"style": "digital", "seconds": True}},
    },
    "standby-digital-12h": {
        "display": {"timeout_s": 2},
        "standby": {"clock": {"style": "digital", "seconds": True}},
        "locale": {"hour12": True},
    },
}

DOOR = {
    "nid": "door",
    "title": "Front door",
    "message": "Someone is at the door",
    "icon": "door",
    "priority": "high",
    "duration_s": 0,
    "actions": [{"id": "open", "label": "Open"}, {"id": "ignore", "label": "Ignore"}],
}
LEAK = {
    "nid": "leak",
    "title": "Water leak",
    "message": "The sensor under the kitchen sink is wet. Turn the water off at the main valve"
    " and check the pipes under the sink before using it again.",
    "icon": "water",
    "priority": "critical",
}
LOW = [
    {
        "nid": "wash",
        "title": "Washing machine",
        "message": "The wash has finished",
        "priority": "low",
    },
    {"nid": "bins", "message": "Bins go out tonight", "priority": "low"},
]

# Notifications for the screens that need some: each after so many seconds.
NOTICES = {
    "notice": [(0, DOOR)],
    "notice-critical": [(0, LEAK)],
    "home-notifications": [(0, n) for n in LOW],
    "notifications": [(0, n) for n in LOW],
}

# The "accents" screen gets a room of its own, named with letters beyond ASCII,
# so the fonts' coverage shows: Spanish, Esperanto, the degree sign.
ACCENTS = (
    "Salón comedor",
    [
        {
            "key": "a",
            "kind": "light",
            "name": "Lámpara del salón",
            "icon": "lamp",
            "features": {"brightness": True},
        },
        {"key": "b", "kind": "switch", "name": "Ĉambro ĝardeno"},
        {"key": "c", "kind": "sensor", "name": "Ŝranko ŭ", "features": {"unit": "°C"}},
    ],
    {"a": {"on": True, "brightness": 35}, "b": {"on": True}, "c": {"value": "21,5"}},
)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def serve_demo(port: int, process: subprocess.Popen, name: str) -> None:
    """Pairs with the panel and serves the demo room until the panel exits."""
    for _ in range(100):
        try:
            client = await NestoClient.connect("127.0.0.1", port)
            break
        except OSError:
            await asyncio.sleep(0.1)
    else:
        raise RuntimeError("the panel never started listening")

    async def accept(code: str) -> bool:
        return True

    key = await client.pair(accept)
    await client.close()
    client = await NestoClient.connect("127.0.0.1", port)
    await client.authenticate(key)
    try:
        if name == "accents":
            await client.synchronise(*ACCENTS)
            while True:
                await client.receive()
        room = DemoRoom()
        room.config = CONFIGS.get(name)
        room.notices = NOTICES.get(name, [])
        await room.serve(client)
    except (asyncio.IncompleteReadError, ConnectionError):
        pass  # the panel saved its screenshot and quit


def take(name: str, script: str | None, out: Path, panel: list[str]) -> None:
    port = free_port()
    with tempfile.TemporaryDirectory() as state:
        command = [*panel, "--headless", "--no-mdns", "--port", str(port)]
        command += ["--state-dir", state, "--panel-id", "0a1b2c3d4e5f"]
        command += ["--script", script or "w1500", "--screenshot", str(out / f"{name}.png")]
        process = subprocess.Popen(command, stderr=subprocess.PIPE, text=True)
        if script:
            asyncio.run(serve_demo(port, process, name))
        _, log = process.communicate(timeout=60)
    if process.returncode != 0:
        sys.exit(f"{name}: the panel failed:\n{log}")
    print(f"{name}.png")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("outdir", type=Path)
    parser.add_argument("--panel", default="/build/sim/nesto", help="how to run the panel")
    parser.add_argument("--only", nargs="*", choices=sorted(SCREENS), help="only these screens")
    args = parser.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)
    for name, script in SCREENS.items():
        if not args.only or name in args.only:
            take(name, script, args.outdir, shlex.split(args.panel))


if __name__ == "__main__":
    main()
