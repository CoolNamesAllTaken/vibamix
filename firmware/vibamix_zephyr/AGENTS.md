# AGENTS.md — vibamix_zephyr

## What this is

`vibamix_zephyr` is an **nRF Connect SDK (NCS) / Zephyr** application for the custom
**"Xiao NRF54L15 ePaper badge"** (`pants_for_birds,vibamix-xiao-cpuapp`). It boots, draws
"Hello World" on an ePaper display, then runs as a connectable BLE peripheral named
`vibamix`.

This app was forked from Nordic's `peripheral_lbs` sample. As a result:

- **`README.rst` is stale boilerplate** describing the original Nordic DK sample, not this
  board. Ignore it.
- The Bluetooth LED Button Service (LBS) is **not actually wired up** despite the lineage —
  see [BLERadio.cpp](src/BLERadio.cpp). It is just a plain connectable advertiser.
- `firmware/vibamix/` (sibling dir) is the **legacy, non-Zephyr** variant. Out of scope; do
  not edit it for Zephyr work.

## Target

- **SoC:** Nordic nRF54L15, Cortex-M33 application core (`cpuapp`). RISC-V FLPR
  (`cpuflpr`) board variants exist under `boards/.../` but are unused by this app.
- **Board id:** `vibamix_xiao/nrf54l15/cpuapp`, vendor `pants_for_birds`. Defined **in-tree**
  at [boards/pants_for_birds/vibamix_xiao/](boards/pants_for_birds/vibamix_xiao/);
  `BOARD_ROOT` is set to the app dir in [CMakeLists.txt](CMakeLists.txt) so the board
  resolves locally with no extra flags.
- **Module:** Seeed **XIAO nRF54L15** (`XIAO-nRF54l15-SMD`) soldered onto a custom carrier
  PCB (KiCad project at `kicad/vibamix_xiao/`).

## Layout

| Path | What |
|------|------|
| [src/](src/) | App code: `main.cpp`, `GUI.*`, `BLERadio.*` |
| [boards/pants_for_birds/vibamix_xiao/](boards/pants_for_birds/vibamix_xiao/) | In-tree board: DTS, pinctrl, defconfig, `board.cmake`, `support/` |
| [dts/bindings/display/](dts/bindings/display/) | Custom `pants-for-birds,epaper` binding |
| `../peripherals/epd/` | **ePaper driver lib (outside the app)** — `Display_EPD_W21*`, `GUI_Paint`, fonts; pulled in via `add_subdirectory` |
| [sysbuild/](sysbuild/) | MCUboot + IPC radio sysbuild config |
| `boards/.../support/` | `openocd.cfg`, `probe-rs-wrapper` |
| `.vscode/` (repo root) | nRF Connect ext config, debug task + launch |

App source:

- [main.cpp](src/main.cpp) — inits GUI (ePaper "Hello World" → deep sleep), configures the
  user LED + button (the button ISR only `printk`s), then starts BLE advertising and sleeps.
- [GUI.cpp](src/GUI.cpp) / [GUI.h](src/GUI.h) — thin C++ wrapper over the EPD driver. Owns
  the framebuffer (`EPD_WIDTH*EPD_HEIGHT/8` bytes); the `GUI` instance is file-scope
  `static` in `main.cpp` to keep that buffer in `.bss` rather than on the main stack.
- [BLERadio.cpp](src/BLERadio.cpp) / [BLERadio.h](src/BLERadio.h) — BLE peripheral.
  File-scope `BT_CONN_CB_DEFINE` callbacks dispatch to a singleton instance; advertising
  auto-restarts on disconnect; the user LED is lit while connected.

## Build / flash / debug

Run from `firmware/vibamix_zephyr/` in an NCS/Zephyr west environment:

```sh
west build -b vibamix_xiao/nrf54l15/cpuapp        # sysbuild pulls in MCUboot + IPC radio
west flash                                         # uses probe-rs runner by default
```

- Output ELF: `build/vibamix_zephyr/zephyr/zephyr.elf`.
- **Debug probe:** the on-module **CMSIS-DAP** of the Seeed XIAO, over SWD (chip selector
  `nRF54L15`). A SWD TagConnect (TC2030-NL) footprint is also on the carrier.
- **Runners** (configured in [board.cmake](boards/pants_for_birds/vibamix_xiao/board.cmake)):
  `probe-rs` (default), `openocd`, `jlink`, `nrfutil`/`nrfjprog`. Pick one with
  `west flash -r <runner>`.
- **`support/probe-rs-wrapper`** expands a bare probe serial into probe-rs's
  `VID:PID:Serial` form (the nRF Connect VS Code extension passes only a serial).
- **`support/openocd.cfg`** uses CMSIS-DAP/SWD and defines a custom `nrf54l-load` proc that
  writes `0x101` to `0x5004b500` (RRAM write-enable) before loading.

VS Code debug (cortex-debug):

- `.vscode/tasks.json` launches a **probe-rs GDB server** on `localhost:1337`.
- `.vscode/launch.json` ("Debug vibamix") attaches, loads, resets, and breaks at `main`.
- ⚠️ The probe serial `6F643EB8` in `tasks.json` is **one specific board** — change it to
  your own probe's serial (`probe-rs list`) when working on a different machine/unit.
- `firmware/scripts/open_nrf54l15_jlink.bash` is a J-Link GDB-server alternative.

## Hardware map

Source of truth: [vibamix_xiao_nrf54l15_cpuapp.dts](boards/pants_for_birds/vibamix_xiao/vibamix_xiao_nrf54l15_cpuapp.dts)
+ [vibamix_xiao-pinctrl.dtsi](boards/pants_for_birds/vibamix_xiao/vibamix_xiao-pinctrl.dtsi),
cross-checked against the schematic in `kicad/vibamix_xiao/`.

| Peripheral | Bus | Pins | Driver / compatible | Status |
|-----------|-----|------|---------------------|--------|
| ePaper display (250×122, GDEY027T91-class) | spi20 @ 4 MHz | SCK P2.01, MOSI P2.02 (no MISO), CS P2.07, D/C P2.08, RST P2.09, BUSY P2.10 | `pants-for-birds,epaper` (custom) | working |
| WS2812B RGB LED | spi21 | DIN P1.06 (MOSI only; SCK P1.05 is NC) | `worldsemi,ws2812-spi`, `chain-length=1` | working |
| LED power gate | GPIO | `led_enable` P1.07, active-low (drives a PMOS) | `gpio-leds` | working |
| Ambient light sensor (LTR-329ALS-01) | i2c20 | SDA P1.10, SCL P1.11 | — | **bus enabled, sensor node TODO** |
| User LED | GPIO | P2.00, active-low — alias `user-led` | `gpio-leds` | working |
| User button | GPIO | P0.00, active-low, external pullup — alias `user-button` | `gpio-keys` | working |
| Console / logging UART | uart22 @ 115200 | TX P1.09, RX P1.08 | `nordic,nrf-uarte` | working |

Aliases used by the app: `user-led`, `user-button`, `epaper`, `led-strip`.

**Flash map** (`cpuapp_rram`, sysbuild includes MCUboot):

| Partition | Offset | Size |
|-----------|--------|------|
| mcuboot | `0x0` | 64K |
| image-0 (slot0) | `0x10000` | 664K |
| image-1 (slot1) | `0xb6000` | 664K |
| storage | `0x15c000` | 36K |

Key Kconfig ([prj.conf](prj.conf)): `CONFIG_BT` + `BT_PERIPHERAL`, `CONFIG_CPP`,
`CONFIG_SPI`, `CONFIG_GPIO`, UART console. Board defconfig enables ARM MPU, cache
management, and the GRTC syscounter.

## Conventions & gotchas

- **C++** is enabled (`CONFIG_CPP`); app modules are classes (`GUI`, `BLERadio`).
- BLE uses a **singleton-dispatch** pattern: file-scope C callbacks forward to the one
  `BLERadio` instance via a `static` pointer.
- The display framebuffer is intentionally a `static` (`.bss`) member, not stack-allocated.
- The **LBS GATT service is not connected** despite the `peripheral_lbs` heritage; the
  button ISR currently only prints.
- The **light sensor is not yet driven** — the i2c20 bus is up but the child node/driver is
  a TODO in the DTS.
- `chain-length=1` for the WS2812 strip is a placeholder; update it to the real LED count.
- The ePaper SPI driver gracefully stubs out when the `epaper` DT node is absent (see
  [Display_EPD_W21_spi.cpp](../peripherals/epd/epaper_display/Display_EPD_W21_spi.cpp)).

## Where to make changes

- **Pin / bus / peripheral wiring** → board DTS + pinctrl in
  `boards/pants_for_birds/vibamix_xiao/`.
- **ePaper low-level / drawing primitives** → `../peripherals/epd/` (**not** `src/`).
- **App-level display content** → [GUI.cpp](src/GUI.cpp).
- **BLE behavior** (name, advertising, connection handling) → [BLERadio.cpp](src/BLERadio.cpp)
  and `CONFIG_BT_*` in [prj.conf](prj.conf).
- **Add the light sensor** → add a child node under `&i2c20` in the DTS with the right
  `compatible` + address, enable the matching `CONFIG_*` driver in `prj.conf`.
