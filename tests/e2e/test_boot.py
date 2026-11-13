"""End-to-end: started by the boot script, with --hold-for-stock, holding the
display in the first seconds hands the thermostat back to the stock software
(scripts/nesto-boot.sh, docs/DEVICE.md)."""

from __future__ import annotations

from pathlib import Path

from conftest import Panel


def test_holding_the_display_as_it_starts_exits_for_the_stock_software(tmp_path: Path) -> None:
    panel = Panel(tmp_path / "state", ["--no-mdns", "--hold-for-stock", "--script", "w300 h2000"])
    panel.start()
    try:
        assert panel.wait_exit(10) == 3
        assert any("the display was held as Nesto started" in line for line in panel.lines)
    finally:
        panel.stop()


def test_a_short_press_or_a_later_hold_doesn_t(tmp_path: Path) -> None:
    # A press in the first seconds; a hold after them.
    steps = "w300 p w3500 h2000 w500"
    panel = Panel(tmp_path / "state", ["--no-mdns", "--hold-for-stock", "--script", steps])
    panel.start()
    try:
        assert panel.wait_exit(8) is None
        assert not any("was held" in line for line in panel.lines)
    finally:
        panel.stop()
