"""Starts a real panel for the end-to-end tests.

The panel runs headless: NESTO_PANEL_CMD says how, for instance
`/build/sim/nesto --headless`, or the ARM build under QEMU. Each test gets its
own panel, port and state directory, and can restart it to check what
survives a restart.
"""

from __future__ import annotations

import os
import re
import shlex
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

PANEL_ID = "0a1b2c3d4e5f"
# The panel's version, as CMakeLists.txt sets it.
VERSION = re.search(
    r"project\(nesto VERSION ([0-9.]+)",
    (Path(__file__).resolve().parents[2] / "CMakeLists.txt").read_text(),
).group(1)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Panel:
    """A panel process, with its log collected as it runs."""

    def __init__(self, state_dir: Path, extra_args: list[str] | None = None) -> None:
        self.state_dir = state_dir
        self.extra_args = extra_args or []
        self.port = free_port()
        self.lines: list[str] = []
        self._process: subprocess.Popen[str] | None = None

    def start(self) -> None:
        """Starts the panel and waits until it listens."""
        command = shlex.split(os.environ.get("NESTO_PANEL_CMD", "/build/sim/nesto --headless"))
        command += ["--port", str(self.port), "--state-dir", str(self.state_dir)]
        command += ["--panel-id", PANEL_ID, *self.extra_args]
        self.lines = []
        self._process = subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True
        )
        threading.Thread(target=self._collect, daemon=True).start()
        self.wait_for_log(r"listening on port", timeout=30)

    def stop(self) -> None:
        if self._process and self._process.poll() is None:
            self._process.terminate()
            try:
                self._process.wait(10)
            except subprocess.TimeoutExpired:
                self._process.kill()
        self._process = None

    def wait_exit(self, timeout: float = 10) -> int | None:
        """The exit code once the panel exits by itself, or None if it's still
        running after `timeout` seconds."""
        assert self._process
        try:
            return self._process.wait(timeout)
        except subprocess.TimeoutExpired:
            return None

    def restart(self) -> None:
        self.stop()
        self.start()

    def _collect(self) -> None:
        assert self._process and self._process.stderr
        for line in self._process.stderr:
            self.lines.append(line.rstrip("\n"))

    def wait_for_log(self, pattern: str, timeout: float = 10) -> re.Match[str]:
        """The first log line matching `pattern`, waiting for it if needed."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in list(self.lines):
                if match := re.search(pattern, line):
                    return match
            if self._process and self._process.poll() is not None:
                break
            time.sleep(0.05)
        log = "\n".join(self.lines)
        raise AssertionError(f"no log line matching {pattern!r}; the panel logged:\n{log}")


@pytest.fixture
def panel(tmp_path: Path):
    p = Panel(tmp_path / "state")
    p.start()
    yield p
    p.stop()
    # pytest shows this only for failing tests.
    print("Panel log:\n" + "\n".join(p.lines))
