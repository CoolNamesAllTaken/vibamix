"""Static, hand-edited configuration for one flashing bench.

Bench-specific slot mapping is populated by ``python -m flashtool.calibrate``
and stored in ``flashtool/usb_hubs/``. Everything else can be edited here.
"""

from __future__ import annotations

import json
from pathlib import Path

# --- Firmware artifacts ------------------------------------------------------
ARTIFACT_DIR: Path = Path("/opt/vibamix/firmware")
FIRMWARE_GLOB: str = "*.hex"
CHIP: str = "nRF54L15"

# --- Flashing backend (probe-rs over the XIAO onboard CMSIS-DAP) -------------
PROBE_RS_BIN: str = "probe-rs"
PROBE_RS_WRAPPER: str | None = None
FLASH_TIMEOUT_S: int = 120

# --- Concurrency -------------------------------------------------------------
MAX_CONCURRENT: int = 20

# --- Slots -------------------------------------------------------------------
# Slots per hub.  MAX_HUBS is the upper bound pre-allocated in the GUI grid;
# the operator selects the actual count at runtime via the hub spinbox.
MAX_SLOTS: int = 20
MAX_HUBS: int = 4

# --- Which USB devices are our programmers -----------------------------------
PROBE_VID_PIDS: set[tuple[int, int]] = {
    (0x2886, 0x0066),  # Seeed XIAO CMSIS-DAP
}

# --- Bench config ------------------------------------------------------------
_CONFIG_DIR: Path = Path(__file__).parent / "usb_hubs"


def list_bench_configs() -> list[Path]:
    """Return all bench config files in _CONFIG_DIR, named ones then bench.json."""
    if not _CONFIG_DIR.exists():
        return []
    named = sorted(_CONFIG_DIR.glob("bench_*.json"))
    default = _CONFIG_DIR / "bench.json"
    if default.exists() and default not in named:
        named.append(default)
    return named


def config_display_name(path: Path) -> str:
    """Strip the ``bench_`` prefix and ``.json`` suffix for human display."""
    stem = path.stem
    return stem[len("bench_"):] if stem.startswith("bench_") else stem


def _abs_to_relative(slot_map: dict[int, str]) -> dict[int, str]:
    """Convert absolute slot_map paths to hub-relative paths.

    ``"1-4.7.1"`` → ``"7.1"``  (strips the ``bus-rootport.`` prefix).
    """
    if not slot_map:
        return {}
    sample = next(iter(slot_map.values()))
    root = sample.split(".", 1)[0]  # e.g. "1-4"
    result: dict[int, str] = {}
    for k, path in slot_map.items():
        result[k] = path[len(root) + 1:] if path.startswith(root + ".") else path
    return result


def load_bench_config_from(path: Path) -> dict[int, str] | None:
    """Return the relative_map from a bench config file, or None if missing.

    Accepts both the current ``relative_map`` format and the legacy
    ``slot_map`` (absolute paths) format, converting the latter automatically.
    """
    if not path.exists():
        return None
    data = json.loads(path.read_text())

    if "relative_map" in data:
        raw = data["relative_map"]
        return {int(k): v for k, v in raw.items()}

    if "slot_map" in data:
        raw = data["slot_map"]
        slot_map = {int(k): v for k, v in raw.items()}
        return _abs_to_relative(slot_map)

    return None


def _find_bench_config() -> Path:
    """Return the best bench config on startup (most recently modified named file)."""
    candidates = sorted(_CONFIG_DIR.glob("bench_*.json")) if _CONFIG_DIR.exists() else []
    if len(candidates) == 1:
        return candidates[0]
    if len(candidates) > 1:
        return max(candidates, key=lambda p: p.stat().st_mtime)
    return _CONFIG_DIR / "bench.json"


_BENCH_CONFIG: Path = _find_bench_config()
RELATIVE_MAP: dict[int, str] | None = load_bench_config_from(_BENCH_CONFIG)
