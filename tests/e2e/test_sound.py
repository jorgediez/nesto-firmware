"""End-to-end: the beeper (PROTOCOL.md sections 12, 14 and 18).

The simulator has no speaker: it logs each tone and click, which these
tests read.
"""

from __future__ import annotations

from pathlib import Path

from test_notifications import serve


def tones(lines: list[str]) -> list[str]:
    return [line.split("] ", 1)[-1] for line in lines if "] tone " in line]


def clicks(lines: list[str]) -> int:
    return sum("] click " in line for line in lines)


def test_a_notification_beeps(tmp_path: Path) -> None:
    notice = {"nid": "a", "message": "Hello", "duration_s": 1}
    _, lines = serve(tmp_path, [(0.5, notice)], "w100", seconds=4)
    assert tones(lines) == ["tone 2500 Hz", "tone off"]


def test_silenced_notifications_beep_only_when_critical(tmp_path: Path) -> None:
    config = {"sound": {"notifications": False}}
    normal = {"nid": "a", "message": "Hello", "duration_s": 1}
    critical = {"nid": "b", "message": "Leak", "priority": "critical", "beep": "alert"}
    _, lines = serve(tmp_path, [(0.5, normal), (1.5, critical)], "w100", seconds=4, config=config)
    assert tones(lines)[0] == "tone 3200 Hz"  # only the critical one's alert
    assert "tone 2500 Hz" not in tones(lines)


def test_the_ring_clicks_but_not_when_it_wakes(tmp_path: Path) -> None:
    # A click of the ring while active, then, after the timeout, one that
    # wakes the display.
    config = {"display": {"timeout_s": 5}}
    _, lines = serve(tmp_path, [], "r+1 w6000 r+1 w300", seconds=12, config=config)
    assert clicks(lines) == 1
    assert "click 2000 Hz, 2000 us" in "\n".join(lines)


def test_pressing_clicks_only_when_turned_on(tmp_path: Path) -> None:
    _, lines = serve(tmp_path, [], "p w300", seconds=6)
    assert clicks(lines) == 0
    config = {"sound": {"press_click": True, "ring_click": False}}
    (tmp_path / "b").mkdir()
    _, lines = serve(tmp_path / "b", [], "p w300 r+1 w300", seconds=6, config=config)
    assert clicks(lines) == 1


def test_identify_beeps_three_times(tmp_path: Path) -> None:
    room, lines = serve(tmp_path, [(0.5, {"t": "identify"})], "w100", seconds=5)
    assert [r["ok"] for r in room.notify_results] == [True]
    assert tones(lines).count("tone 2500 Hz") == 3
