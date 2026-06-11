"""Unit tests for usb_topology — no hardware required."""

from __future__ import annotations

import pytest

from flashtool import config
from flashtool.models import Status, UsbDevice
from flashtool import usb_topology


class FakeDev:
    def __init__(self, bus: int, port_numbers: tuple[int, ...],
                 vid: int = 0x2E8A, pid: int = 0x000C,
                 serial: str | None = "DEADBEEF",
                 product: str | None = "XIAO nRF54L15"):
        self.bus = bus
        self.port_numbers = port_numbers
        self.idVendor = vid
        self.idProduct = pid
        self._serial = serial
        self._product = product

    @property
    def iSerialNumber(self) -> int:
        return 3

    @property
    def iProduct(self) -> int:
        return 2


# ---------------------------------------------------------------------------
# port_path
# ---------------------------------------------------------------------------

def test_port_path_single_port():
    assert usb_topology.port_path(FakeDev(1, (2,))) == "1-2"

def test_port_path_chain():
    assert usb_topology.port_path(FakeDev(1, (1, 4, 3))) == "1-1.4.3"

def test_port_path_empty_ports():
    assert usb_topology.port_path(FakeDev(2, ())) == "2-"


# ---------------------------------------------------------------------------
# path_sort_key
# ---------------------------------------------------------------------------

def test_path_sort_key_numeric_order():
    paths = ["1-2.1.10", "1-2.1.9", "1-2.1.2", "1-2.2.1"]
    assert sorted(paths, key=usb_topology.path_sort_key) == [
        "1-2.1.2", "1-2.1.9", "1-2.1.10", "1-2.2.1"
    ]

def test_path_sort_key_cross_controller():
    paths = ["1-2.2.1", "1-2.1.10", "1-2.1.1"]
    assert sorted(paths, key=usb_topology.path_sort_key) == [
        "1-2.1.1", "1-2.1.10", "1-2.2.1"
    ]


# ---------------------------------------------------------------------------
# hub_root / strip_hub_root
# ---------------------------------------------------------------------------

def test_hub_root():
    assert usb_topology.hub_root("1-4.7.1") == "1-4"

def test_hub_root_single_level():
    assert usb_topology.hub_root("1-4") == "1-4"

def test_strip_hub_root():
    assert usb_topology.strip_hub_root("1-4.7.1") == "7.1"

def test_strip_hub_root_deep():
    assert usb_topology.strip_hub_root("1-4.1.2.3") == "1.2.3"


# ---------------------------------------------------------------------------
# find_hub_roots
# ---------------------------------------------------------------------------

def test_find_hub_roots_single():
    probes = [UsbDevice("1-4.7.1", 0x2886, 0x0066, None),
              UsbDevice("1-4.7.2", 0x2886, 0x0066, None)]
    assert usb_topology.find_hub_roots(probes) == ["1-4"]

def test_find_hub_roots_multiple():
    probes = [UsbDevice("1-4.7.1", 0x2886, 0x0066, None),
              UsbDevice("1-5.7.1", 0x2886, 0x0066, None)]
    assert usb_topology.find_hub_roots(probes) == ["1-4", "1-5"]

def test_find_hub_roots_sorted():
    probes = [UsbDevice("1-10.1.1", 0x2886, 0x0066, None),
              UsbDevice("1-2.1.1",  0x2886, 0x0066, None)]
    assert usb_topology.find_hub_roots(probes) == ["1-2", "1-10"]

def test_find_hub_roots_empty():
    assert usb_topology.find_hub_roots([]) == []


# ---------------------------------------------------------------------------
# UsbDevice.selector
# ---------------------------------------------------------------------------

def test_selector_format():
    dev = UsbDevice(port_path="1-1.1", vid=0x2E8A, pid=0x000C, serial="ABC123")
    assert dev.selector() == "2e8a:000c:ABC123"


# ---------------------------------------------------------------------------
# build_slots
# ---------------------------------------------------------------------------

def test_build_slots_single_hub():
    rel = {0: "7.1", 1: "7.2", 2: "1.1"}
    slots = usb_topology.build_slots(rel, ["1-4"], num_hubs=1)
    assert len(slots) == 3
    assert [s.port_path for s in slots] == ["1-4.7.1", "1-4.7.2", "1-4.1.1"]

def test_build_slots_two_hubs():
    rel = {0: "7.1", 1: "7.2"}
    slots = usb_topology.build_slots(rel, ["1-4", "1-5"], num_hubs=2)
    assert len(slots) == 4
    assert slots[0].port_path == "1-4.7.1"
    assert slots[1].port_path == "1-4.7.2"
    assert slots[2].port_path == "1-5.7.1"
    assert slots[3].port_path == "1-5.7.2"

def test_build_slots_indices():
    rel = {0: "7.1", 1: "7.2"}
    slots = usb_topology.build_slots(rel, ["1-4", "1-5"], num_hubs=2)
    assert [s.index for s in slots] == [0, 1, 2, 3]

def test_build_slots_placeholder_when_hub_missing():
    rel = {0: "7.1", 1: "7.2"}
    # only one hub detected but num_hubs=2
    slots = usb_topology.build_slots(rel, ["1-4"], num_hubs=2)
    assert slots[0].port_path == "1-4.7.1"
    assert slots[2].port_path == "hub1.7.1"  # placeholder

def test_build_slots_all_absent():
    rel = {0: "7.1", 1: "7.2"}
    slots = usb_topology.build_slots(rel, ["1-4"], num_hubs=1)
    assert all(s.status is Status.ABSENT for s in slots)
    assert all(s.device is None for s in slots)

def test_build_slots_no_map_placeholders(monkeypatch):
    monkeypatch.setattr(config, "MAX_SLOTS", 2)
    slots = usb_topology.build_slots(None, None, num_hubs=1)
    assert len(slots) == 2
    assert slots[0].port_path == "port-1"


# ---------------------------------------------------------------------------
# bind_devices
# ---------------------------------------------------------------------------

def test_bind_device_present():
    rel = {0: "7.1", 1: "7.2"}
    slots = usb_topology.build_slots(rel, ["1-4"], num_hubs=1)
    probe = UsbDevice("1-4.7.2", 0x2E8A, 0x000C, "SER002")
    usb_topology.bind_devices(slots, [probe])
    assert slots[1].device is probe
    assert slots[1].status is Status.IDLE

def test_bind_absent_slot_stays_absent():
    rel = {0: "7.1", 1: "7.2"}
    slots = usb_topology.build_slots(rel, ["1-4"], num_hubs=1)
    probe = UsbDevice("1-4.7.2", 0x2E8A, 0x000C, "SER002")
    usb_topology.bind_devices(slots, [probe])
    assert slots[0].status is Status.ABSENT

def test_bind_multiple_hubs():
    rel = {0: "7.1", 1: "7.2"}
    slots = usb_topology.build_slots(rel, ["1-4", "1-5"], num_hubs=2)
    probes = [
        UsbDevice("1-4.7.1", 0x2E8A, 0x000C, "AAA"),
        UsbDevice("1-5.7.2", 0x2E8A, 0x000C, "BBB"),
    ]
    usb_topology.bind_devices(slots, probes)
    assert slots[0].device.serial == "AAA"   # hub 0, slot 0
    assert slots[1].status is Status.ABSENT  # hub 0, slot 1
    assert slots[2].status is Status.ABSENT  # hub 1, slot 0
    assert slots[3].device.serial == "BBB"   # hub 1, slot 1

def test_bind_unmapped_path_ignored():
    rel = {0: "7.1"}
    slots = usb_topology.build_slots(rel, ["1-4"], num_hubs=1)
    probe = UsbDevice("1-9.9.9", 0x2E8A, 0x000C, "UNK")
    usb_topology.bind_devices(slots, [probe])
    assert all(s.device is None for s in slots)


# ---------------------------------------------------------------------------
# Determinism
# ---------------------------------------------------------------------------

def test_port_determines_slot_not_serial():
    rel = {0: "7.1", 1: "7.2"}
    slots = usb_topology.build_slots(rel, ["1-4"], num_hubs=1)
    usb_topology.bind_devices(slots, [
        UsbDevice("1-4.7.1", 0x2E8A, 0x000C, "BOARD_A"),
        UsbDevice("1-4.7.2", 0x2E8A, 0x000C, "BOARD_B"),
    ])
    assert slots[0].device.serial == "BOARD_A"
    assert slots[1].device.serial == "BOARD_B"

    for s in slots:
        s.device = None
        s.status = Status.ABSENT
    usb_topology.bind_devices(slots, [
        UsbDevice("1-4.7.1", 0x2E8A, 0x000C, "BOARD_B"),
        UsbDevice("1-4.7.2", 0x2E8A, 0x000C, "BOARD_A"),
    ])
    assert slots[0].device.serial == "BOARD_B"
    assert slots[1].device.serial == "BOARD_A"


# ---------------------------------------------------------------------------
# find_hub_roots_from_map
# ---------------------------------------------------------------------------

def test_find_hub_roots_from_map_direct():
    """Two hubs directly on different laptop ports."""
    rel = {0: "7.1", 1: "7.2"}
    probes = [UsbDevice("1-4.7.1", 0x2886, 0x0066, None),
              UsbDevice("1-5.7.2", 0x2886, 0x0066, None)]
    assert usb_topology.find_hub_roots_from_map(probes, rel) == ["1-4", "1-5"]

def test_find_hub_roots_from_map_behind_dock():
    """Two identical hubs behind a shared dock: same bus-port, distinct deeper roots."""
    rel = {0: "7.1", 1: "7.2"}
    probes = [UsbDevice("1-4.3.7.1", 0x2886, 0x0066, None),
              UsbDevice("1-4.4.7.2", 0x2886, 0x0066, None)]
    assert usb_topology.find_hub_roots_from_map(probes, rel) == ["1-4.3", "1-4.4"]

def test_find_hub_roots_from_map_ignores_unmatched():
    """A board whose relative path is not in the map does not create a root."""
    rel = {0: "7.1"}
    probes = [UsbDevice("1-4.7.1", 0x2886, 0x0066, None),
              UsbDevice("1-4.9.9", 0x2886, 0x0066, None)]
    assert usb_topology.find_hub_roots_from_map(probes, rel) == ["1-4"]

def test_find_hub_roots_from_map_empty_probes():
    rel = {0: "7.1", 1: "7.2"}
    assert usb_topology.find_hub_roots_from_map([], rel) == []

def test_find_hub_roots_from_map_fallback_empty_map():
    """Falls back to find_hub_roots when relative_map is empty."""
    probes = [UsbDevice("1-4.7.1", 0x2886, 0x0066, None)]
    assert usb_topology.find_hub_roots_from_map(probes, {}) == ["1-4"]

def test_find_hub_roots_from_map_single_hub_multiple_boards():
    """Multiple boards on one hub produce a single root entry."""
    rel = {0: "7.1", 1: "7.2", 2: "1.1"}
    probes = [UsbDevice("1-4.7.1", 0x2886, 0x0066, None),
              UsbDevice("1-4.7.2", 0x2886, 0x0066, None),
              UsbDevice("1-4.1.1", 0x2886, 0x0066, None)]
    assert usb_topology.find_hub_roots_from_map(probes, rel) == ["1-4"]


# ---------------------------------------------------------------------------
# enumerate_probes
# ---------------------------------------------------------------------------

def test_enumerate_filters_by_vid_pid(monkeypatch):
    monkeypatch.setattr(config, "PROBE_VID_PIDS", {(0x2E8A, 0x000C)})
    fake_devices = [
        FakeDev(1, (1,), vid=0x2E8A, pid=0x000C, serial="PROBE"),
        FakeDev(1, (2,), vid=0x1234, pid=0x5678, serial="OTHER"),
    ]
    import usb.core, usb.util
    monkeypatch.setattr(usb.core, "find", lambda **kw: iter(fake_devices))
    monkeypatch.setattr(usb.util, "get_string", lambda dev, idx: dev._serial)
    probes = usb_topology.enumerate_probes()
    assert len(probes) == 1
    assert probes[0].serial == "PROBE"

def test_enumerate_tolerates_serial_read_error(monkeypatch):
    monkeypatch.setattr(config, "PROBE_VID_PIDS", {(0x2E8A, 0x000C)})
    fake_devices = [FakeDev(1, (1,), vid=0x2E8A, pid=0x000C)]
    import usb.core, usb.util
    monkeypatch.setattr(usb.core, "find", lambda **kw: iter(fake_devices))
    monkeypatch.setattr(usb.util, "get_string",
                        lambda dev, idx: (_ for _ in ()).throw(Exception("denied")))
    probes = usb_topology.enumerate_probes()
    assert len(probes) == 1
    assert probes[0].serial is None
