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
}


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def serve_demo(port: int, process: subprocess.Popen) -> None:
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
        await DemoRoom().serve(client)
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
            asyncio.run(serve_demo(port, process))
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
