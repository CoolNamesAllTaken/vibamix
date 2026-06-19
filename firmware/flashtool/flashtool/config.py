"""Static, hand-edited configuration for one flashing bench.

Bench-specific slot mapping is populated by ``python -m flashtool.calibrate``
and stored in ``flashtool/usb_hubs/``. Everything else can be edited here.
"""

from __future__ import annotations

import json
from pathlib import Path

# --- NCS / west build --------------------------------------------------------
# Root of the nRF Connect SDK installation.  The toolchain subdirectory is
# resolved automatically from toolchains.json; override NCS_TOOLCHAIN_DIR if
# the auto-detection ever breaks.
NCS_BASE: Path = Path.home() / "ncs"
NCS_VERSION: str = "v3.3.1"
NCS_DIR: Path = NCS_BASE / NCS_VERSION

# Zephyr app source and default build output directory.
WEST_APP_DIR: Path = Path(__file__).parent.parent.parent / "vibamix_zephyr"
BOARD: str = "vibamix_xiao/nrf54l15/cpuapp"
BUILD_DIR: Path = WEST_APP_DIR / "build"

# First-stage direct-XIP bootloader: a separate Zephyr app linked at 0x0, built
# into its own dir under the app build tree (see builder.build_bootloader).
BOOTLOADER_SRC_DIR: Path = WEST_APP_DIR.parent / "bootloader"
BOOTLOADER_BUILD_DIR: Path = BUILD_DIR / "bl"

# Resolved at import time from toolchains.json; falls back to the first
# toolchain dir found under NCS_BASE/toolchains/.
def _find_toolchain_dir() -> Path:
    tj = NCS_BASE / "toolchains" / "toolchains.json"
    if tj.exists():
        data = json.loads(tj.read_text())
        # toolchains.json may be a list of objects or a single object.
        items = data if isinstance(data, list) else [data]
        for item in items:
            if not isinstance(item, dict):
                continue
            for entry in item.get("toolchains", []):
                if NCS_VERSION in entry.get("ncs_versions", []):
                    bid = entry["identifier"]["bundle_id"]
                    return NCS_BASE / "toolchains" / bid
    # fallback: pick the most recently modified toolchain directory
    tc_dir = NCS_BASE / "toolchains"
    candidates = [d for d in tc_dir.iterdir() if d.is_dir()] if tc_dir.exists() else []
    return max(candidates, key=lambda d: d.stat().st_mtime) if candidates \
        else NCS_BASE / "toolchains" / "unknown"

NCS_TOOLCHAIN_DIR: Path = _find_toolchain_dir()

# --- Firmware artifacts ------------------------------------------------------
# A factory board needs BOTH images flashed over SWD (custom direct-XIP A/B
# bootloader, no MCUboot): the bootloader at 0x0 and the app linked into slot A
# at 0xE000.  These are the plain ``west build`` outputs for each.
ARTIFACT_DIR: Path = BUILD_DIR / "zephyr"             # app build output dir
FIRMWARE_GLOB: str = "zephyr.hex"
APP_HEX: Path = ARTIFACT_DIR / "zephyr.hex"           # app, slot A @ 0xE000
BOOTLOADER_HEX: Path = BOOTLOADER_BUILD_DIR / "zephyr" / "zephyr.hex"  # BL @ 0x0
CHIP: str = "nRF54L15"

# --- Flashing backend (probe-rs over the XIAO onboard CMSIS-DAP) -------------
# probe-rs must be installed separately (e.g. via `cargo install probe-rs-tools`
# or a pre-built binary on PATH).  Set PROBE_RS_BIN to an absolute path if it
# is not on PATH.
PROBE_RS_BIN: str = "probe-rs"
PROBE_RS_WRAPPER: str | None = None   # set to wrapper script path if needed
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

# --- Per-unit factory id assignment ------------------------------------------
# Each board is assigned a unique 15-bit id (1..0x7FFF) written to the `factory`
# flash partition; the firmware uses it as the mesh unicast address / config code
# / GAP name so a large fleet doesn't collide (see src/factory_id.c).  The
# registry persists the next id + a probe-serial->id map so re-flashing a board
# reuses its id.  BACK THIS FILE UP — losing it restarts ids at 1 and risks
# duplicate ids on already-flashed badges.
FACTORY_ID_ADDR: int = 0x164000              # start of the `factory` partition
FACTORY_ID_MAX: int = 0x7FFF                 # 15-bit mesh unicast ceiling
SERIAL_REGISTRY: Path = _CONFIG_DIR / "serials.json"


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
