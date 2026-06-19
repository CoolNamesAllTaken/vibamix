"""Per-unit badge id assignment + persistence (ported from flashtool).

Each physical board gets a unique 15-bit id (1..0x7FFF) that the firmware uses as
its mesh unicast address / config code / GAP name. A random FICR-id slice collides
at ~75% for 300 badges, so we assign ids explicitly.

The registry is keyed by the probe's CMSIS-DAP serial so re-flashing the same
board reuses its id (idempotent retries). It is the source of truth for "which
ids are taken" — see flashconfig.SERIAL_REGISTRY and BACK IT UP.
"""

from __future__ import annotations

import json
import threading

from . import flashconfig

_lock = threading.Lock()


def _load() -> dict:
    try:
        data = json.loads(flashconfig.SERIAL_REGISTRY.read_text())
    except (FileNotFoundError, ValueError):
        return {"next_id": 1, "assigned": {}}
    data.setdefault("next_id", 1)
    data.setdefault("assigned", {})
    return data


def _save(data: dict) -> None:
    flashconfig.SERIAL_REGISTRY.parent.mkdir(parents=True, exist_ok=True)
    flashconfig.SERIAL_REGISTRY.write_text(json.dumps(data, indent=2) + "\n")


def assign_for(serial: str | None) -> int:
    """Return this board's id, allocating the next sequential id on first sight.

    Thread-safe: called from the flashing thread pool. ``serial`` is the probe's
    CMSIS-DAP serial (the stable per-board key). Raises RuntimeError on a missing
    serial (can't key the registry) or when the 15-bit id space is exhausted.
    """
    if not serial:
        raise RuntimeError("probe has no serial — cannot assign a stable factory id")
    with _lock:
        data = _load()
        existing = data["assigned"].get(serial)
        if existing is not None:
            return int(existing)

        new_id = int(data["next_id"])
        if new_id > flashconfig.FACTORY_ID_MAX:
            raise RuntimeError(
                f"badge id space exhausted (>{flashconfig.FACTORY_ID_MAX:#06x})")
        data["assigned"][serial] = new_id
        data["next_id"] = new_id + 1
        _save(data)
        return new_id
