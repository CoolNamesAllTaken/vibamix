"""Static config for the SWD bulk-flash feature (Flash tab).

Folds in the core of the standalone ``firmware/flashtool`` — probe-rs flashing of
the bootloader + a *bootable* app — without its USB-hub topology / calibration.
See ``flash.py`` (backend), ``probes.py`` (enumeration), ``serialreg.py`` (ids).
"""

from __future__ import annotations

from pathlib import Path

# --- target / tool -----------------------------------------------------------
CHIP: str = "nRF54L15"
# probe-rs must be installed separately (e.g. `cargo install probe-rs-tools` or a
# pre-built binary). Set an absolute path here if it is not on PATH.
PROBE_RS_BIN: str = "probe-rs"
FLASH_TIMEOUT_S: int = 120

# Flash all attached probes in parallel; one worker per probe.
MAX_CONCURRENT: int = 20

# Per-probe retry: 20 CMSIS-DAP probes attaching on one hub at the same instant
# produces transient probe-rs attach/comm failures. Retry the whole flash a few
# times (idempotent — assign_for is keyed by serial, download only erases written
# sectors), and stagger worker starts so they don't all hit the bus at once.
FLASH_MAX_ATTEMPTS: int = 3
FLASH_RETRY_BACKOFF_S: float = 2.0   # sleep backoff_s * attempt + jitter between tries
STAGGER_STEP_S: float = 0.3          # worker N waits N * step before its first attempt

# --- default firmware artifacts ----------------------------------------------
# Resolved relative to the repo so the Flash tab pre-fills the paths that
# ``vibamix_zephyr/scripts/build_slots.sh`` produces. The operator builds first
# (we point at pre-built outputs); both can be overridden in the UI.
#
# flashconfig.py -> badgectl/badgectl -> badgectl -> firmware
_FIRMWARE_DIR: Path = Path(__file__).resolve().parent.parent.parent
_ZEPHYR_BUILD: Path = _FIRMWARE_DIR / "vibamix_zephyr" / "build"

# Bootloader: a Zephyr .hex (carries its own 0x0 base address) -> flashed as hex.
DEFAULT_BOOTLOADER_HEX: Path = _ZEPHYR_BUILD / "bl" / "zephyr" / "zephyr.hex"
# App slot A: the *trailered* binary so the bootloader's CRC check passes and the
# board cold-boots standalone -> flashed as `bin` at the slot-A offset below.
DEFAULT_APP_BIN: Path = _ZEPHYR_BUILD / "slotA.bin"

# --- flash map (see vibamix_xiao DTS / firmware/README.md) --------------------
APP_SLOT_A_ADDR: int = 0xE000        # image-a partition (app0) offset

# --- per-unit factory id (mesh unicast / config code / GAP name) -------------
# Each board gets a unique 15-bit id written to the `factory` partition so a
# fleet doesn't collide (src/factory_id.c). The registry (serialreg.py) keys ids
# by probe serial and persists them — BACK IT UP.
FACTORY_ID_ADDR: int = 0x164000      # start of the `factory` partition
FACTORY_ID_MAX: int = 0x7FFF         # 15-bit mesh unicast ceiling
# 8-byte record: u32 magic 'VBXF' (LE), u16 id, u16 ~id (see factory_id.h).
FACTORY_ID_MAGIC: int = 0x46584256

# Persisted alongside the existing ~/.badgectl/seq.json (seqstore.py).
SERIAL_REGISTRY: Path = Path.home() / ".badgectl" / "serials.json"
