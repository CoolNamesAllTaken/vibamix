# vibamix firmware — build & flash

The badge runs two images on the nRF54L15:

- **`bootloader/`** — a custom first-stage **direct-XIP A/B bootloader** at flash `0x0`. On boot it
  jumps to the app: straight to slot A if a debugger is attached, otherwise to the highest-version
  slot whose CRC trailer verifies (with a watchdog-guarded trial/confirm for safe OTA).
- **`vibamix_zephyr/`** — the application, linked into **slot A** by default. It confirms its own
  slot early in `main()` (`ota_confirm_on_boot()`), so a normally-booting app "sticks."

There is **no MCUboot / sysbuild** — always build with `--no-sysbuild` so the DTS partitions drive
linking (see below).

> Related tools: [`badgectl/`](badgectl/README.md) (laptop GUI: GATT config, mesh, **BLE OTA**),
> [`flashtool/`](flashtool/README.md) (bulk USB flashing), and the BLE/OTA wire spec in
> [`vibamix_zephyr/docs/ble-config-api.md`](vibamix_zephyr/docs/ble-config-api.md).

## Flash map (`cpuapp_rram`, from the board DTS)

| Region | Offset | Size | Notes |
|--------|--------|------|-------|
| bootloader | `0x0` | 48 KB | first-stage BL (flashed once) |
| bl_state | `0xC000` | 8 KB | per-slot OTA state |
| **slot A** (`image-a`) | `0xE000` | 512 KB | app (default `zephyr,code-partition`) |
| slot B (`image-b`) | `0x8E000` | 512 KB | OTA alternate |
| images | `0x10E000` | 80 KB | 5 stored badge image slots (4 user + identity) |
| storage | `0x122000` | 264 KB | settings (ZMS) |

## 0. Toolchain environment

VS Code users can skip this — the tasks in [`../.vscode/tasks.json`](../.vscode/tasks.json) set it.
For the command line (NCS v3.3.0; adjust the paths if your SDK/toolchain version differs):

```sh
export TC=/opt/nordic/ncs/toolchains/0c0f19d91c
export PATH="$TC/bin:$TC/usr/bin:$TC/usr/local/bin:$TC/opt/bin:$TC/opt/nanopb/generator-bin:$TC/nrfutil/bin:$TC/opt/zephyr-sdk/arm-zephyr-eabi/bin:$TC/opt/zephyr-sdk/riscv64-zephyr-elf/bin:$PATH"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$TC/opt/zephyr-sdk"
export ZEPHYR_BASE=/opt/nordic/ncs/v3.3.0/zephyr
export NRFUTIL_HOME="$TC/nrfutil/home"
```

`west flash` uses the **probe-rs** runner (chip `nRF54L15`) over the XIAO's on-board CMSIS-DAP.

## 1. Build (run from this `firmware/` directory)

`--no-sysbuild` and `-DBOARD_ROOT` are **required**: sysbuild's Partition Manager would ignore the
DTS partitions and link the app to fill all of flash.

```sh
# First-stage bootloader  -> build/bl/zephyr/zephyr.{elf,hex}
west build -p always -b vibamix_xiao/nrf54l15/cpuapp --no-sysbuild \
  -d build/bl bootloader -- -DBOARD_ROOT=vibamix_zephyr

# Application (slot A)     -> build/slotA/zephyr/zephyr.{elf,bin}
west build -p always -b vibamix_xiao/nrf54l15/cpuapp --no-sysbuild \
  -d build/slotA vibamix_zephyr -- -DBOARD_ROOT=vibamix_zephyr
```

(For OTA you also need the app linked for slot B — see the all-in-one script in step 4c.)

## 2. Flash the bootloader — once per board

```sh
west flash --erase -d build/bl
```

`--erase` wipes the whole chip (clears any stale partitions). The BL **persists across app
reflashes** — you normally do this only once per board. In VS Code this is the
**"Flash bootloader (one-time)"** task: run it from **Terminal → Run Task…** (note: `Cmd+Shift+B`
only runs the *default* build task, and the nRF Connect "Actions" panel doesn't list `tasks.json`
tasks, which is why you won't find it there).

## 3. Get the app running — pick one

### a) Debug / dev loop (VS Code, fastest)
Press **F5** ("Debug vibamix"). The debugger `load`s the unsigned app ELF into slot A, resets, and
the BL — seeing the attached debugger — jumps straight to slot A and breaks at `main`. **No trailer
needed**; reset/restart re-breaks at `main`. This is the everyday inner loop once the BL is flashed.

### b) Standalone over SWD (no debugger)
For power-on boot, the BL only runs an image with a valid **CRC trailer**, so append one and write it
to slot A (`0xE000`):

```sh
python3 vibamix_zephyr/scripts/vbx_trailer.py \
  build/slotA/zephyr/zephyr.bin build/slotA.bin --version $(date +%s)
probe-rs download --chip nRF54L15 --binary-format bin --address 0xe000 build/slotA.bin
```

On power-up the BL verifies the trailer, trial-boots slot A, and the app confirms itself
(`ota_confirm_on_boot()`), so it survives the watchdog. **A plain `west flash` of the app (no
trailer) will run under a debugger but will NOT cold-boot** — use the trailered image above.

### c) OTA over BLE (no wires — via badgectl)
Build both slots + an OTA bundle, then push it from the laptop:

```sh
cd vibamix_zephyr && bash scripts/build_slots.sh   # -> build/slotA.bin, build/slotB.bin, build/vibamix.ota
```

Then in [`badgectl`](badgectl/README.md): wake + connect to the badge and run the OTA action with
`build/vibamix.ota`. badgectl reads which slot is *inactive*, streams that image, and the badge
verifies it, reboots into it as a trial, and the app confirms it. Wire format: see
[`docs/ble-config-api.md`](vibamix_zephyr/docs/ble-config-api.md) §10.

> **Two OTA gotchas** (see §10 "Recovery, rollback & boot diagnostics"):
> 1. **Detach the debugger before OTA.** With a probe attached the BL boots slot A (the old image)
>    no matter what, so the new slot silently won't run.
> 2. **Auto‑revert needs a trailered fallback.** An **F5**‑loaded app has no CRC trailer, so it's not
>    a valid rollback target. SWD‑flash the trailered `build/slotA.bin` (step **b**), not F5, if you
>    want a real revert target. On a trial/revert/halt the BL now draws a **diagnostic screen** on the
>    ePaper (chosen slot + per‑slot valid/CRC/version/attempts) — read it if a badge sticks post‑update.

## Notes

- **`--no-sysbuild` is mandatory** for both images (direct-XIP linking from the DTS partitions).
- The bootloader is flashed once; slot B exists only for OTA.
- `--erase` is only for the bootloader / first flash — don't erase when updating the app, or you'll
  wipe the BL and stored settings/images.
- Trailer format and slot logic live in [`common/vbx_img.h`](common/vbx_img.h),
  [`common/bl_state.h`](common/bl_state.h), and [`bootloader/src/main.c`](bootloader/src/main.c).
