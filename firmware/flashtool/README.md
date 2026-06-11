# flashtool

A desktop app for flashing a pre-built firmware image onto every device plugged
into a USB hub, concurrently, with a live per-device status UI.

## What it does

- Enumerates every vibamix programmer on one USB hub.
- Binds each **physical hub port** to a fixed **slot number** in the UI, shown
  as a 2×10 grid matching the hub's physical layout.
- Flashes a pre-built artifact (from a static directory) to all present devices
  at once, each in its own worker thread.
- Shows live status per slot (absent / ready / flashing / done / error).

## Design notes

### Determinism: ports, not serials

Slots are keyed on **USB topology** — the bus number plus the chain of port
numbers from the root hub (e.g. `"1-2.1.3"`). That path belongs to the physical
socket, so the same port always maps to the same slot regardless of which board
is seated or what order devices enumerate at power-on.

pyusb (libusb) exposes this cross-platform via `device.bus` and
`device.port_numbers`. A serial number is used only as the probe-rs `--probe`
selector *after* a device has been bound to its slot by port.

### Explicit slot map

Large hubs often have multiple internal controllers, so USB paths don't follow
a simple `prefix.1`…`prefix.20` sequence. The tool instead builds an explicit
`slot_map` (stored in `bench.json`) by asking you to unplug and replug the hub
with all boards seated. Detected paths are sorted numerically — which matches
physical port order for common hub designs — and assigned slots 0–19.

The map lives in `bench.json` (gitignored, machine-specific). Re-run
calibration if the hub is moved to a different USB port on the laptop.

### Flashing backend

`flasher.py` shells out to **probe-rs** over each board's onboard CMSIS-DAP.
The backend is isolated behind one function so it can be swapped for nrfutil /
J-Link later.

### Concurrency

The GUI runs on the main thread. `Flash All` spawns a dispatcher thread that
fans work out to a `ThreadPoolExecutor` (`config.MAX_CONCURRENT`). Workers
report progress by posting events back to the GUI via
`window.write_event_value` (thread-safe in FreeSimpleGUI).

## Layout

```
flashtool/
  config.py         # bench setup: artifact dir, chip, VID/PID, slot limits
  models.py         # UsbDevice, Slot, Status
  usb_topology.py   # enumerate hub, bind ports to slots, sort helpers
  flasher.py        # probe-rs backend: flash one device + parse progress
  controller.py     # discovery + concurrent dispatch (no GUI deps)
  gui.py            # FreeSimpleGUI 2x10 card grid + event loop
  calibrate.py      # `python -m flashtool.calibrate` -> writes bench.json
  __main__.py       # `python -m flashtool` -> launches the GUI
bench.json          # machine-specific slot map (gitignored)
```

## Setup

**System dependencies** (install before `poetry install`):

```bash
# Debian/Ubuntu:
sudo apt install python3-tk libusb-1.0-0

# macOS (tkinter bundled with python.org installer):
brew install libusb
```

```bash
cd firmware/flashtool
poetry install
```

## Calibrate the slot map (once per bench)

Calibration identifies which USB path corresponds to each physical hub port.
It is a **one-time task** per bench, or whenever the hub is moved to a
different USB port on the laptop. The result is saved to `bench.json`.

### Option A — GUI (recommended)

Launch the app and click **Discover**. The button walks you through the same
unplug/replug cycle as the CLI tool, writes `bench.json`, and immediately
refreshes the slot display.

```bash
poetry run python -m flashtool
```

### Option B — CLI

```bash
poetry run python -m flashtool.calibrate
```

**Step 1** — Have one board ready (or 20 — either works).

**Step 2** — Follow the per-slot prompts. For each of the 20 slots the tool
asks you to plug a board into that physical port, detects the USB path, and
moves on:

```
flashtool hub calibration

Slot 1 of 20: plug a board into physical slot 1, then press Enter…
> [plug in, press Enter]
  slot  0  ->  1-2.1.1

Slot 2 of 20: plug a board into physical slot 2, then press Enter…
> [move board to slot 2, press Enter]
  slot  1  ->  1-2.1.2
...
  slot 19  ->  1-2.2.10

Final map (20 slots):
  slot  0  ->  1-2.1.1
  ...

Write this to bench.json? [y/N]
```

With one board: unplug it from the previous slot and move it to the next before
pressing Enter. With multiple boards: use a fresh board for each slot and leave
them all in.

**Step 3** — Press `y`. The slot map is written to `bench.json`.

### Verifying the map

Plug one board in at a time and watch which slot lights up green in the GUI to
confirm it matches the physical label on the hub.

### Re-calibrating

Only needed if:
- the hub is moved to a **different USB port on the laptop** (changes the topology prefix), or
- boards are **rearranged** across ports (slot assignments are port-based, not board-based).

Plugging the hub back into the **same** laptop port after a reboot is fine — the path is stable across reboots.

## Run

```bash
cd firmware/flashtool
poetry run python -m flashtool
```

## Verify VID/PID

If no boards appear after calibration, confirm the VID:PID in `config.py`
matches your hardware:

```bash
lsusb          # Linux
probe-rs list  # cross-platform
```

The Seeed XIAO nRF54L15 CMSIS-DAP probe is `0x2886:0x0066` (already set as the
default).
