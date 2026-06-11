"""Enumerate USB hubs and bind each physical port to a fixed slot.

Slots are keyed on USB topology: bus + port chain ("1-2.1.3"). The bench
config stores *relative* paths (hub-root stripped, e.g. "7.1") so the same
config works for every instance of the same hub model regardless of which
laptop port it is plugged into.

At runtime, detected boards are grouped by hub root ("1-4", "1-5", …),
sorted deterministically, and assigned to consecutive slot banks
(hub 0 → slots 0–N-1, hub 1 → slots N–2N-1, …).
"""

from __future__ import annotations

import usb.core
import usb.util

from . import config
from .models import Slot, Status, UsbDevice


def port_path(dev: "usb.core.Device") -> str:
    """Return the stable topology id ``"bus-p.p.p"`` for a pyusb device."""
    ports = dev.port_numbers or ()
    return f"{dev.bus}-{'.'.join(str(p) for p in ports)}"


def path_sort_key(path: str) -> tuple[int, ...]:
    """Numeric sort key for a USB path so "1-2.1.10" > "1-2.1.9"."""
    return tuple(int(n) for n in path.replace("-", ".").split(".") if n)


def hub_root(path: str) -> str:
    """Return the top-level port component of a path.

    ``"1-4.7.1"`` → ``"1-4"``
    """
    return path.split(".", 1)[0]


def strip_hub_root(path: str) -> str:
    """Remove the hub root prefix from an absolute path.

    ``"1-4.7.1"`` → ``"7.1"``
    """
    parts = path.split(".", 1)
    return parts[1] if len(parts) > 1 else path


def enumerate_probes() -> list[UsbDevice]:
    """Find every CMSIS-DAP programmer currently attached."""
    found = []
    for dev in usb.core.find(find_all=True):
        if not dev.port_numbers:
            continue
        if (dev.idVendor, dev.idProduct) not in config.PROBE_VID_PIDS:
            continue
        try:
            serial = usb.util.get_string(dev, dev.iSerialNumber)
        except Exception:
            serial = None
        try:
            product = usb.util.get_string(dev, dev.iProduct)
        except Exception:
            product = None
        found.append(UsbDevice(port_path(dev), dev.idVendor, dev.idProduct,
                               serial, product))
    return found


def find_hub_roots(probes: list[UsbDevice]) -> list[str]:
    """Return sorted unique hub root paths derived from a list of probes.

    E.g. probes at ``1-4.7.1`` and ``1-5.1.3`` → ``["1-4", "1-5"]``.
    """
    roots: set[str] = {hub_root(p.port_path) for p in probes}
    return sorted(roots, key=path_sort_key)


def find_hub_roots_from_map(
    probes: list[UsbDevice],
    relative_map: dict[int, str],
) -> list[str]:
    """Discover hub instances by matching probe paths against the relative_map.

    Uses the depth of relative-map values to infer where each probe's hub root
    ends: everything before the last N path segments (N = relative-path depth)
    is the hub root. This correctly handles hubs connected at any nesting depth,
    including multiple hubs behind a shared dock or intermediate hub.

    Only roots with at least one probe landing on a mapped relative path are
    returned. Falls back to ``find_hub_roots`` when ``relative_map`` is empty.
    """
    if not relative_map:
        return find_hub_roots(probes)
    rel_depth = len(next(iter(relative_map.values())).split("."))
    rel_values = set(relative_map.values())
    roots: set[str] = set()
    for probe in probes:
        parts = probe.port_path.split(".")
        hub_depth = len(parts) - rel_depth
        if hub_depth < 1:
            continue
        root = ".".join(parts[:hub_depth])
        rel_path = ".".join(parts[hub_depth:])
        if rel_path in rel_values:
            roots.add(root)
    return sorted(roots, key=path_sort_key)


def build_slots(
    relative_map: dict[int, str] | None = None,
    hub_roots: list[str] | None = None,
    num_hubs: int = 1,
) -> list[Slot]:
    """Create one Slot per (hub instance × port).

    ``relative_map`` maps slot index → hub-relative path (e.g. ``"7.1"``).
    ``hub_roots`` is the list of detected hub root paths (e.g. ``["1-4","1-5"]``);
    if fewer are detected than ``num_hubs``, the remaining banks use placeholder
    paths so the GUI always shows the expected number of rows.
    """
    slots_per_hub = len(relative_map) if relative_map else config.MAX_SLOTS
    roots = list(hub_roots or [])

    slots: list[Slot] = []
    for h in range(num_hubs):
        root = roots[h] if h < len(roots) else f"hub{h}"
        for i in range(slots_per_hub):
            global_idx = h * slots_per_hub + i
            if relative_map:
                path = f"{root}.{relative_map[i]}"
            else:
                path = f"port-{global_idx + 1}"
            slots.append(Slot(index=global_idx, port_path=path))
    return slots


def bind_devices(slots: list[Slot], probes: list[UsbDevice]) -> None:
    """Attach each probe to its slot by matching ``port_path``.

    Bound slots become IDLE; unmatched slots stay ABSENT.
    """
    by_path = {s.port_path: s for s in slots}
    for p in probes:
        slot = by_path.get(p.port_path)
        if slot is None:
            continue
        slot.device = p
        slot.status = Status.IDLE


def snapshot_all_port_paths() -> set[str]:
    """Return the topology path of every USB device currently visible."""
    return {port_path(d) for d in usb.core.find(find_all=True) if d.port_numbers}


def diff_hub_ports(before: set[str], after: set[str]) -> set[str]:
    """Return ports that appeared in ``after`` but were absent in ``before``."""
    return after - before
