# CLAUDE.md — vibamix

The **firmware architecture is documented in AGENTS.md**, imported here so it's in context by
default. Treat it as authoritative for anything under `firmware/vibamix_zephyr/`:

@firmware/vibamix_zephyr/AGENTS.md

## Repo layout

- `firmware/` — all the embedded + tooling code:
  - `vibamix_zephyr/` — the **badge application** (nRF54L15 / NCS Zephyr). Architecture: AGENTS.md (above).
  - `bootloader/` — custom direct-XIP A/B first-stage bootloader (flashed once over SWD).
  - `common/` — headers shared by the app **and** the bootloader (`vbx_img.h` trailer, `bl_state.*`).
  - `peripherals/` — out-of-app driver libs (ePaper `epd/`, `qrcodegen/`) pulled in via `add_subdirectory`.
  - `badgectl/` — PyQt6 laptop tool: GATT config, mesh-TX gateway client (inject mesh via the
    `f0de000C` char on a config-mode badge), **BLE OTA**.
  - `flashtool/` — bulk USB flashing utility.
  - `scripts/` — misc helper scripts (e.g. J-Link GDB server).
- `kicad/` — hardware (the `vibamix_xiao` carrier PCB).
- `3d/`, `affinity/`, `reference/` — enclosure/3D, design assets, reference material.

## Source-of-truth pointers

- **Build / flash / OTA** → [firmware/README.md](firmware/README.md). Always build with
  `--no-sysbuild` + `-DBOARD_ROOT` (direct-XIP linking from the DTS partitions).
- **Firmware internals** → [AGENTS.md](firmware/vibamix_zephyr/AGENTS.md) (authoritative).
- **GATT + OTA wire spec (byte layouts)** → [docs/ble-config-api.md](firmware/vibamix_zephyr/docs/ble-config-api.md).
- **Laptop tool** → [badgectl/README.md](firmware/badgectl/README.md).
