"""End-to-end: Home Assistant's own discovery library finds the panel.

python-zeroconf is what Home Assistant's zeroconf integration uses, so if it
browses, resolves and connects here, ha-nesto's discovery will too.
"""

from __future__ import annotations

import asyncio
import threading
import time

from conftest import PANEL_ID, VERSION, Panel
from test_panel import pair
from zeroconf import ServiceBrowser, ServiceInfo, ServiceStateChange, Zeroconf

SERVICE_TYPE = "_nesto._tcp.local."
INSTANCE = f"Nesto Panel {PANEL_ID[-6:]}.{SERVICE_TYPE}"


def resolve(timeout: float = 10) -> ServiceInfo:
    """Resolves the panel with a fresh Zeroconf, so nothing comes from a cache."""
    zc = Zeroconf()
    try:
        info = zc.get_service_info(SERVICE_TYPE, INSTANCE, timeout=int(timeout * 1000))
    finally:
        zc.close()
    assert info is not None, f"{INSTANCE} didn't resolve"
    return info


def test_the_panel_is_found_by_browsing(panel: Panel) -> None:
    found = threading.Event()
    names: list[str] = []

    def on_change(zeroconf, service_type, name, state_change) -> None:
        if state_change is ServiceStateChange.Added:
            names.append(name)
            found.set()

    zc = Zeroconf()
    try:
        ServiceBrowser(zc, SERVICE_TYPE, handlers=[on_change])
        assert found.wait(10), "no _nesto._tcp service was found"
    finally:
        zc.close()
    assert INSTANCE in names


def test_the_service_resolves_to_the_panel(panel: Panel) -> None:
    info = resolve()
    assert info.port == panel.port
    assert info.server == f"nesto-{PANEL_ID[-6:]}.local."
    properties = {k.decode(): v.decode() for k, v in info.properties.items()}
    assert properties == {
        "id": PANEL_ID,
        "proto": "1",
        "fw": VERSION,
        "model": "nest-gen2",
        "paired": "0",
    }
    addresses = info.parsed_addresses()
    assert addresses, "no IPv4 address in the answer"

    # The address and port from discovery reach the panel.
    from nesto_client import NestoClient

    async def connect() -> dict:
        client = await NestoClient.connect(addresses[0], info.port)
        await client.close()
        return client.hello

    assert asyncio.run(connect())["panel_id"] == PANEL_ID


def test_pairing_is_announced(panel: Panel) -> None:
    assert resolve().properties[b"paired"] == b"0"
    asyncio.run(pair(panel))

    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if resolve().properties[b"paired"] == b"1":
            return
        time.sleep(0.5)
    raise AssertionError("the TXT record still says paired=0")


def test_no_mdns_means_no_answer(tmp_path) -> None:
    quiet = Panel(tmp_path / "state", extra_args=["--no-mdns"])
    quiet.start()
    try:
        zc = Zeroconf()
        try:
            assert zc.get_service_info(SERVICE_TYPE, INSTANCE, timeout=2000) is None
        finally:
            zc.close()
    finally:
        quiet.stop()
