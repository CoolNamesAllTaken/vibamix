# AGENTS.md — vibamix_zephyr

## What this is

`vibamix_zephyr` is an **nRF Connect SDK (NCS) / Zephyr** application for the custom
**"Xiao NRF54L15 ePaper badge"** (`pants_for_birds,vibamix-xiao-cpuapp`). Each badge is a node
in a **zero-config Bluetooth Mesh network** — an event attendee wears one, and an operator can,
from a phone in range of **any** badge, push config to **one** badge or to **all** badges at
once: attendee name, fun facts, LED color, and a full-screen ePaper image.

How the pieces map onto Bluetooth Mesh:

- **Managed-flooding relay** — a message injected at one badge is rebroadcast across the whole
  mesh (`CONFIG_BT_MESH_RELAY`).
- **GATT Proxy ingress** — a phone/laptop BLE central connects to the nearest badge over the
  Mesh Proxy GATT service and injects messages (`CONFIG_BT_MESH_GATT_PROXY` + `PB_GATT`).
- **Addressing** — send to a badge's **unicast address** = one badge; send to the shared group
  **`0xC000`** = all badges. (`0xFFFF` all-nodes also works for broadcast.)
- **Vendor model** — a custom model carries the config + image opcodes (see below).
- **Zero-config** — every badge ships with the **same baked-in net/app/dev keys** and
  self-provisions at boot (no PB-ADV commissioning). The controller app must be seeded with the
  same keys. Security tradeoff is intentional — see [mesh_keys.h](src/mesh_keys.h).

History / out-of-scope:

- This app was forked from Nordic's `peripheral_lbs` sample; the old plain-BLE `BLERadio`
  advertiser has been **replaced** by the mesh node (`MeshNode` + `mesh_model`).
- **`README.rst` is stale boilerplate** describing the original Nordic DK sample. Ignore it.
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
| [src/](src/) | App code: `main.cpp`, `GUI.*`, `LEDStrip.*`, `MeshNode.*`, `mesh_model.*`, `image_xfer.*`, `app_config.*`, `mesh_keys.h` |
| [boards/pants_for_birds/vibamix_xiao/](boards/pants_for_birds/vibamix_xiao/) | In-tree board: DTS, pinctrl, defconfig, `board.cmake`, `support/` |
| [dts/bindings/display/](dts/bindings/display/) | Custom `pants-for-birds,epaper` binding |
| `../peripherals/epd/` | **ePaper driver lib (outside the app)** — `Display_EPD_W21*`, `GUI_Paint`, fonts; pulled in via `add_subdirectory` |
| [sysbuild/](sysbuild/) | MCUboot + IPC radio sysbuild config |
| `boards/.../support/` | `openocd.cfg`, `probe-rs-wrapper` |
| `.vscode/` (repo root) | nRF Connect ext config, debug task + launch |

### C vs C++ boundary

The mesh stack glue is in **C** files, the rest of the app is **C++**. This is mandatory:
`BT_MESH_MODEL_*` use C99 compound literals that don't compile as C++. So composition data,
the element/model tables, and opcode handlers live in `mesh_model.c` / `image_xfer.c` /
`app_config.c`, and they call back into the C++ app (`MeshNode`) through function pointers.
The C headers all have `extern "C"` guards.

App source:

- [main.cpp](src/main.cpp) — inits GUI (boot "Hello World" → deep sleep), configures the user
  LED + button (button ISR only `printk`s), brings up the mesh node (`MeshNode::init`), inits
  the LED strip, applies any persisted identity/color, then runs the LED render loop.
- [MeshNode.cpp](src/MeshNode.cpp) / [.h](src/MeshNode.h) — **C++ owner of the mesh node**
  (replaces the old `BLERadio`). Does `bt_enable` → `bt_mesh_init` → `settings_load` →
  deterministic self-provision → bind app key + subscribe group → `bt_mesh_prov_enable(GATT)`.
  A file-scope singleton + `extern "C"` trampolines route vendor-model callbacks to instance
  methods that drive `GUI`/`LEDStrip`/`app_config`.
- [mesh_model.c](src/mesh_model.c) / [.h](src/mesh_model.h) — **C**: vendor model, opcode
  table + handlers, Config Server + Health Server, composition data. `mesh_model_comp()` feeds
  `bt_mesh_init`; `mesh_model_bind_and_subscribe()` writes `model.keys[0]`/`model.groups[0]`
  directly (standing in for a Config Client).
- [image_xfer.c](src/image_xfer.c) / [.h](src/image_xfer.h) — **C**: START/DATA/END chunk
  reassembly state machine with a CRC32 check. Writes **directly into the GUI framebuffer**
  (no second 5.8 KB buffer) and fires a completion callback on a verified END.
- [app_config.c](src/app_config.c) / [.h](src/app_config.h) — **C**: persists name / fun fact /
  LED color in the `vibamix` settings subtree (ZMS). Pushed images are **not** persisted (the
  ePaper is bistable and retains its last image; a badge re-renders its name/fact on reboot).
- [mesh_keys.h](src/mesh_keys.h) — baked-in shared net/app/dev keys, net/app indices, and the
  `0xC000` group address. **Change these to rotate the fleet's network.**
- [GUI.cpp](src/GUI.cpp) / [GUI.h](src/GUI.h) — thin C++ wrapper over the EPD driver. Owns the
  framebuffer (`EPD_WIDTH*EPD_HEIGHT/8` = 5808 bytes), exposed via `framebuffer()` for in-place
  image reassembly. Adds `wake()` (re-init after deep sleep), `show_text(name, fun_fact)`
  (name in Font24 + word-wrapped fact in Font16), and `render_image()`. The `GUI` instance is
  file-scope `static` in `main.cpp` to keep that buffer in `.bss`, not on the main stack.
- [LEDStrip.cpp](src/LEDStrip.cpp) / [LEDStrip.h](src/LEDStrip.h) — drives the WS2812 chain via
  a small `LedPattern` abstraction (`Off`/`Solid`/`Rainbow`/`Wheel`). `render()` advances a
  free-running `m_tick` and dispatches to a per-pattern `render_*()`; `set_pattern()` selects one
  and `set_color(r,g,b)` sets a solid brightness-scaled color (e.g. from a mesh command).
  `play_for(ms)` is a blocking boot intro on the same clock. The default is `Wheel` — a
  continuously scrolling RGB wheel with a soft brightness blob that bounces left↔right — which
  runs until a persisted/mesh color switches it to `Solid`. Add a pattern = enum value +
  `render_*()` + a `render()` case.

## Mesh protocol (vendor model)

Company ID `CONFIG_BT_COMPANY_ID` (Nordic `0x0059` for the prototype), vendor model id
`0x0001`. Opcodes are 3-byte (`BT_MESH_MODEL_OP_3`). Defined + handled in
[mesh_model.c](src/mesh_model.c):

| Opcode | Name | Payload |
|--------|------|---------|
| `0x01` | `SET_NAME` | UTF-8 string (attendee name) |
| `0x02` | `SET_FUN_FACT` | UTF-8 string |
| `0x03` | `SET_LED_COLOR` | `r, g, b` (3 bytes) |
| `0x04` | `IMG_START` | `le16 size, le16 width, le16 height` |
| `0x05` | `IMG_DATA` | `le16 offset, bytes[]` |
| `0x06` | `IMG_END` | `le32 crc32` (IEEE) |

**Addressing:** send to a badge's unicast address for one badge, or to group `0xC000`
(`VIBAMIX_GROUP_ADDR`) for all badges. The unicast address is derived deterministically from
the FICR device id in `MeshNode::init` (`sys_get_le16(uuid) & 0x7FFF`).

**Self-provisioning sequence** (`MeshNode::init`): `bt_enable` → `bt_mesh_init(&prov, comp)` →
`settings_load()` → if not already provisioned, `bt_mesh_provision(net_key, …, addr, dev_key)`
+ `bt_mesh_app_key_add` with the baked keys → `mesh_model_bind_and_subscribe(app_idx, 0xC000)`
→ `bt_mesh_prov_enable(BT_MESH_PROV_GATT)`.

**Image transfer reality:** ~56 segmented messages for a 5808-byte frame at
`SEG_MAX=10`. Segmented sends to a **group are unacknowledged** — a dropped segment leaves a
hole, so `image_xfer_end` rejects an incomplete/!CRC frame rather than rendering garbage. For
reliable "image to all," push per-badge unicast (acked) or add an app-level NACK/retransmit
(the controller asks for missing offsets). The ePaper full refresh is ~2 s, so the panel is
redrawn only on a verified `IMG_END`, never per chunk.

## Build / flash / debug

Run from `firmware/vibamix_zephyr/` in an NCS/Zephyr west environment:

```sh
west build -b vibamix_xiao/nrf54l15/cpuapp        # sysbuild (Kconfig.sysbuild)
west flash                                         # uses probe-rs runner by default
```

- ⚠️ Building from the **command line** (not the nRF Connect VS Code extension), the in-tree
  board isn't found by the sysbuild top-level — pass `BOARD_ROOT` explicitly:
  `west build -b vibamix_xiao/nrf54l15/cpuapp -- -DBOARD_ROOT=$(pwd)`. (The app `CMakeLists.txt`
  sets `BOARD_ROOT` for its own image, but sysbuild resolves the board earlier.)
- Output ELF: `build/vibamix_zephyr/zephyr/zephyr.elf` (hex: `…/zephyr.hex`).
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
| ePaper display (GDEY027T91, 176×264) | spi00 @ 4 MHz | SCK P2.01, MOSI P2.02 (no MISO), CS P2.07, D/C P2.08, RST P2.09, BUSY P2.10 | `pants-for-birds,epaper` (custom) | working (on high-speed SERIAL00 to free SERIAL20 for the sensor) |
| WS2812B RGB LEDs (D4–D7, 4-LED chain) | spi21 @ 4 MHz | DIN P1.06 (MOSI only; SCK P1.05 is NC) | `worldsemi,ws2812-spi`, `chain-length=4` | scrolling wheel + blob → solid on mesh `SET_LED_COLOR` (bus **must** be 4 MHz to match the 0x70/0x40 frames) |
| LED power gate | GPIO | `led_enable` P1.07, active-low (drives a PMOS) | `gpio-leds` | working |
| Ambient light sensor (LTR-329ALS-01) | i2c20 | SDA P1.10, SCL P1.11 | — | **bus enabled (SERIAL20, freed by moving ePaper to spi00), sensor node TODO** |
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
| storage | `0x15c000` | 36K | settings (ZMS) — `chosen { zephyr,settings-partition }` |

Mesh build footprint: app image ≈ **24% of the code-flash region (~350 KB)**, **~34% RAM
(~66 KB)** — comfortable headroom on both even with the full mesh + proxy + relay stack.

Key Kconfig ([prj.conf](prj.conf)):

- **Mesh:** `CONFIG_BT_MESH`, `BT_MESH_RELAY` (flood), `BT_MESH_GATT_PROXY` + `BT_MESH_PB_GATT`
  (phone ingress), `BT_OBSERVER`, `BT_PERIPHERAL`. Deliberately **no** `BT_MESH_DK_PROV` —
  badges self-provision, never commissioned over PB-ADV.
- **SAR (image chunking):** `BT_MESH_RX_SEG_MAX=10`, `BT_MESH_TX_SEG_MAX=10`,
  `BT_MESH_RX_SEG_MSG_COUNT=2`, `BT_MESH_MODEL_GROUP_COUNT=2`, `BT_MESH_MODEL_KEY_COUNT=1`.
- **Persistence:** `CONFIG_SETTINGS` + `CONFIG_ZMS` (nRF54L15 uses **ZMS, not NVS**),
  `CONFIG_BT_SETTINGS` (mesh seq/RPL/IV across reboots), `CONFIG_FLASH`/`FLASH_MAP`.
- **Misc:** `CONFIG_HWINFO` (device-id → unicast addr), `CONFIG_CPP`, `SPI`, `GPIO`,
  `LED_STRIP`, UART console, bumped `BT_RX_STACK_SIZE`/`SYSTEM_WORKQUEUE_STACK_SIZE`.

Board defconfig enables ARM MPU, cache management, and the GRTC syscounter. On the single-core
nRF54L15 the whole mesh stack runs on `cpuapp`; the `ipc_radio` sysbuild image is inert here
(it only spawns on multi-core parts), so **no sysbuild change was needed for mesh**.

## Conventions & gotchas

- **Mesh tables must be C, not C++** — `BT_MESH_MODEL_*` use C99 compound literals. Keep
  composition/models/handlers in the `.c` files; reach the C++ app via function pointers
  (`mesh_config_handlers`, `image_complete_cb`). C headers use `extern "C"` guards.
- **C++** is enabled (`CONFIG_CPP`); app modules are classes (`GUI`, `LEDStrip`, `MeshNode`).
- `MeshNode` uses the same **singleton-dispatch** pattern the old `BLERadio` did: file-scope
  `extern "C"` trampolines forward to the one instance via a `static` pointer.
- The display framebuffer is a `static` (`.bss`) member and is **shared** with `image_xfer`
  (received image chunks land straight in it) — don't add a second full-frame buffer.
- **Zero-config = shared keys.** Every badge has the same net/app/dev key
  ([mesh_keys.h](src/mesh_keys.h)); anyone with the key can address the whole fleet. Fine for a
  badge demo, not for real isolation.
- **Unicast address collisions:** the address is a 15-bit slice of the device id, so a large
  fleet can collide. Mitigate with more FICR entropy or a build-time per-unit address.
- **Group image transfers are best-effort** (no SAR ack) — see the Mesh protocol section.
- The ePaper is **bistable**: it keeps its last image with no power, so pushed images survive
  reboot on-screen even though they aren't persisted to settings. `wake()` (re-init) is
  required before any redraw because the panel deep-sleeps after each update.
- The **light sensor is not yet driven** — the i2c23 bus is up but the child node/driver is
  a TODO in the DTS.
- **nRF54L SERIAL blocks are shared:** `spiXX`/`i2cXX`/`uartXX` with the same instance number
  are the *same* SERIAL hardware and are mutually exclusive. `spi20` and `i2c20` would clash, so
  the **ePaper was moved to the free high-speed `spi00`** (it reaches the same P2.01/P2.02 pins),
  leaving SERIAL20 for the `i2c20` light sensor — both run concurrently, no time-multiplexing.
  The sensor pins (P1.10/P1.11) can only be driven by SERIAL20/21/22 (all otherwise occupied),
  and SERIAL00 only reaches the P2 pins the display uses, so moving the *display* (not the
  sensor) is the one routing that works. In use: SERIAL00=ePaper, 20=light sensor, 21=LED strip,
  22=console UART. (This SoC exposes SERIAL 00/20/21/22/30 only — there is no SERIAL23.)
- The WS2812 chain is **4 LEDs** (D4–D7); the strip is power-gated by `led_enable` (P1.07) —
  that gate **must be driven active** (done in `LEDStrip::init`) or the LEDs have no VDD.
- The ePaper SPI driver gracefully stubs out when the `epaper` DT node is absent (see
  [Display_EPD_W21_spi.cpp](../peripherals/epd/epaper_display/Display_EPD_W21_spi.cpp)).

## Where to make changes

- **Pin / bus / peripheral wiring** → board DTS + pinctrl in
  `boards/pants_for_birds/vibamix_xiao/`.
- **ePaper low-level / drawing primitives** → `../peripherals/epd/` (**not** `src/`).
- **App-level display content** (identity screen, image blit) → [GUI.cpp](src/GUI.cpp).
- **LED strip behavior** (colors, animation, brightness) → [LEDStrip.cpp](src/LEDStrip.cpp).
- **New mesh command / opcode** → add to the opcode table + a handler in
  [mesh_model.c](src/mesh_model.c), a `mesh_config_handlers` entry, and wire it in
  [MeshNode.cpp](src/MeshNode.cpp).
- **Mesh network identity / keys / group** → [mesh_keys.h](src/mesh_keys.h).
- **Provisioning / proxy / relay behavior** → [MeshNode.cpp](src/MeshNode.cpp) and the
  `CONFIG_BT_MESH_*` options in [prj.conf](prj.conf).
- **Persisted config (what survives reboot)** → [app_config.c](src/app_config.c).
- **Image transfer protocol / reliability (NACK)** → [image_xfer.c](src/image_xfer.c).
- **Add the light sensor** → add a child node under `&i2c20` in the DTS with the right
  `compatible` + address, enable the matching `CONFIG_*` driver in `prj.conf`.
