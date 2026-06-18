"""Per-unit badge id assignment + persistence.

Each physical board gets a unique 15-bit id (1..0x7FFF) that the firmware uses as
its mesh unicast address / config code / GAP name.  A random device-id slice
collides at ~75% for 300 badges, so we assign ids explicitly here instead.

The registry is keyed by the probe's CMSIS-DAP serial so re-flashing the same
board reuses its id (no waste, idempotent retries).  It is the source of truth
for "which ids are taken" — see config.SERIAL_REGISTRY and back it up.
"""

from __future__ import annotations

import json
import threading

from . import config
from .models import UsbDevice

_lock = threading.Lock()


def _key(dev: UsbDevice) -> str:
    """Stable per-board key: the probe serial, else the physical port path."""
    return dev.serial or f"port:{dev.port_path}"


def _load() -> dict:
    try:
        data = json.loads(config.SERIAL_REGISTRY.read_text())
    except (FileNotFoundError, ValueError):
        return {"next_id": 1, "assigned": {}}
    data.setdefault("next_id", 1)
    data.setdefault("assigned", {})
    return data


def _save(data: dict) -> None:
    config.SERIAL_REGISTRY.parent.mkdir(parents=True, exist_ok=True)
    config.SERIAL_REGISTRY.write_text(json.dumps(data, indent=2) + "\n")


def assign_for(dev: UsbDevice) -> int:
    """Return this board's id, allocating the next sequential id on first sight.

    Thread-safe: called from the flashing thread pool.  Raises RuntimeError if
    the 15-bit id space is exhausted.
    """
    key = _key(dev)
    with _lock:
        data = _load()
        existing = data["assigned"].get(key)
        if existing is not None:
            return int(existing)

        new_id = int(data["next_id"])
        if new_id > config.FACTORY_ID_MAX:
            raise RuntimeError(
                f"badge id space exhausted (>{config.FACTORY_ID_MAX:#06x})")
        data["assigned"][key] = new_id
        data["next_id"] = new_id + 1
        _save(data)
        return new_id
