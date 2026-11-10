"""nesto-probe, the read-only device check, run as dev.sh device-check runs it.

NESTO_PROBE_CMD says how to run it, for instance `/build/nest/nesto-probe`
under QEMU. Here there's no framebuffer and no input devices, so exactly those
two checks fail; everything Nesto needs from the kernel must pass.
"""

from __future__ import annotations

import os
import re
import shlex
import socket
import subprocess
from pathlib import Path

from conftest import free_port

EXPECTED_OK = [
    r"monotonic clock",
    r"allocated and used 8 MiB",
    r"/dev/urandom gives random bytes",
    r"cryptography matches the test vectors",
    r"pairing key saved, read back and private",
    r"TCP port \d+ is free for Nesto",
    r"TCP: non-blocking listen, connect, select, accept and send work",
    r"mDNS: UDP port 5353 can be bound",
]


def probe(*args: str) -> subprocess.CompletedProcess[str]:
    command = shlex.split(os.environ.get("NESTO_PROBE_CMD", "/build/sim/nesto-probe"))
    return subprocess.run(
        [*command, *args], capture_output=True, text=True, timeout=120, check=False
    )


def lines(result: subprocess.CompletedProcess[str], tag: str) -> list[str]:
    return [line[len(tag) :].strip() for line in result.stdout.splitlines() if line.startswith(tag)]


def test_everything_but_the_hardware_passes(tmp_path: Path) -> None:
    result = probe("--port", str(free_port()), "--temp-dir", str(tmp_path))
    ok = lines(result, "ok:")
    for expected in EXPECTED_OK:
        assert any(re.search(expected, line) for line in ok), (expected, result.stdout)
    failed = lines(result, "FAIL:")
    assert len(failed) == 2, result.stdout
    assert failed[0].startswith("framebuffer /dev/fb0")
    assert failed[1].startswith("no input devices")
    assert "2 check(s) failed" in result.stdout
    assert result.returncode == 2


def test_it_leaves_nothing_behind(tmp_path: Path) -> None:
    probe("--port", str(free_port()), "--temp-dir", str(tmp_path))
    assert list(tmp_path.iterdir()) == []


def test_a_taken_port_is_reported_and_the_rest_still_runs(tmp_path: Path) -> None:
    with socket.socket() as taken:
        taken.bind(("0.0.0.0", 0))
        taken.listen()
        port = taken.getsockname()[1]
        result = probe("--port", str(port), "--temp-dir", str(tmp_path))
    failed = lines(result, "FAIL:")
    assert any(f"TCP port {port}:" in line for line in failed), result.stdout
    assert any("select, accept and send work" in line for line in lines(result, "ok:"))
    assert result.returncode == 3


def test_an_unusable_temp_dir_is_a_failure(tmp_path: Path) -> None:
    result = probe("--port", str(free_port()), "--temp-dir", str(tmp_path / "missing"))
    assert any("can't make a temporary directory" in line for line in lines(result, "FAIL:"))


def test_unknown_options_show_the_usage() -> None:
    result = probe("--frobnicate")
    assert result.returncode == 2
    assert result.stdout.startswith("usage: nesto-probe")
