# flashtool

Desktop app for flashing a firmware image onto every vibamix board plugged into
one or more USB hubs at once, with a live per-slot status display.

## Quick start

### 1. Install system dependencies

```bash
# Debian / Ubuntu
sudo apt install python3-tk libusb-1.0-0

# macOS (tkinter ships with the python.org installer)
brew install libusb
```

### 2. Install Python dependencies

```bash
cd firmware/flashtool
pip install poetry       # if not already installed
poetry install
```

### 3. Calibrate your hub (once per hub model)

Calibration maps each physical port on the hub to a slot number. You only need
to do this once per hub **model** — the same config file works on every machine
and on every instance of that hub model, regardless of which USB port it is
plugged into.

```bash
poetry run python -m flashtool.calibrate
```

Follow the prompts: for each slot, plug one board into that physical port and
press Enter. The tool detects the USB path and moves on.

```
flashtool hub calibration (20 slots)

Slot 1 of 20: plug a board into physical slot 1, then press Enter…
  slot  0  ->  7.1
Slot 2 of 20: plug a board into physical slot 2, then press Enter…
  slot  1  ->  7.2
...
Save as (e.g. my_hub): sabrent_20_port
Written to flashtool/usb_hubs/bench_sabrent_20_port.json
```

The result is a **relative** port map (hub-root stripped) stored in
`flashtool/usb_hubs/`. It is committed to the repo so every team member can use
it without re-calibrating.

### 4. Run

```bash
poetry run python -m flashtool
```

The app opens with one column group per detected hub. Plug boards in and watch
slots turn green.

---

## Usage

### Hub selection

Use the **Config** dropdown in the toolbar to select the bench config for your
hub model. If you have multiple identical hubs, the app detects all instances
automatically and shows them as separate column groups. New hubs can be plugged
in while the app is running — the layout expands automatically.

### Flashing

Select a firmware file and click **Flash All**. Every slot that has a board
present will be flashed in parallel. Status per slot:

| Status | Meaning |
|--------|---------|
| `—` | No board in this port |
| `ready` | Board present, not yet flashed |
| `flash…` | Flashing in progress |
| `verify…` | Verifying written data |
| `✓ done` | Flashed successfully |
| `✗ error` | Flash failed — check the board |

### Re-calibrating

You only need to re-calibrate if you get a **different hub model**. Moving the
same hub to a different USB port on the laptop, or plugging it into a different
machine, does not require re-calibration.

To add a new hub model, re-run the calibrate script and save under a new name.

---

## Design notes

### Port-based determinism

Slots are keyed on **USB topology** — the chain of port numbers from the host
controller to the device (e.g. `"1-4.7.1"`). This path belongs to the physical
socket, not the board, so the same port always maps to the same slot regardless
of which board is seated or enumeration order at power-on. A serial number is
used only as the `probe-rs --probe` selector after a device is bound to its slot.

### Portable hub configs

Large hubs have multiple internal controllers, so USB paths don't follow a
simple `prefix.1`…`prefix.N` sequence. Calibration builds an explicit per-slot
map by asking you to plug one board at a time into each port.

The config stores **relative** paths (the hub-root prefix stripped off). At
runtime the app detects where each hub instance sits in the USB tree by matching
board paths against the relative map, strips the right number of segments, and
reconstructs full paths for each hub automatically. The same JSON file drives
every instance of the same hub model on any machine.

### Multi-hub support

Multiple identical hubs are detected automatically. The app groups boards by hub
instance and displays each as a separate column group. The hub count grows as
new instances are detected; removing all boards from a hub keeps its columns
visible (slots go absent) so the layout stays stable while boards are swapped.

### Flashing backend

`flasher.py` shells out to **probe-rs** over each board's onboard CMSIS-DAP.
The backend is isolated behind a single function so it can be swapped for
nrfutil or J-Link later. Flashing runs in a `ThreadPoolExecutor` (bounded by
`config.MAX_CONCURRENT`); workers post progress events back to the GUI thread
via `window.write_event_value`.

---

## Troubleshooting

**No boards appear after plugging in**

Verify the VID:PID in `config.py` matches your hardware:

```bash
lsusb             # Linux
probe-rs list     # cross-platform
```

The Seeed XIAO nRF54L15 CMSIS-DAP probe is `0x2886:0x0066` (default).

On Linux you may also need a udev rule to allow non-root USB access:

```
# /etc/udev/rules.d/99-vibamix.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="2886", ATTRS{idProduct}=="0066", MODE="0666"
```

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

**Boards appear on the wrong slots**

The hub may be connected through an intermediate hub or dock that the previous
calibration didn't account for. Re-run calibration with the hub connected the
same way it will be used in production.

**Slots don't update when boards are plugged in**

The hotplug poll interval is set in `controller.py` (`start_hotplug_monitor`,
default 0.5 s). Reduce it if faster detection is needed.

---

## File layout

```
flashtool/
  config.py          # chip, probe-rs path, artifact dir, concurrency
  models.py          # UsbDevice, Slot, Status
  usb_topology.py    # USB enumeration, hub root detection, slot building
  flasher.py         # probe-rs backend: flash one device, parse progress
  controller.py      # discovery + concurrent flash dispatch (no GUI deps)
  gui.py             # FreeSimpleGUI slot grid + event loop
  calibrate.py       # CLI: build per-slot port map, write bench config
  __main__.py        # entry point: `python -m flashtool`
  usb_hubs/          # bench configs (committed; one file per hub model)
    bench_<name>.json
```
