# badgectl

A laptop GUI to exercise **every** vibamix badge command — the direct **GATT** config
service and the **Bluetooth Mesh** vendor model — for bring-up and demos. Host-side only;
no firmware changes.

## Install & run

```sh
cd firmware/badgectl
poetry install
poetry run badgectl
```

(Needs Python 3.10+. On **macOS**, the first run prompts for Bluetooth permission —
allow it in System Settings → Privacy & Security → Bluetooth. CoreBluetooth hides MAC
addresses, so badges are listed by their advertised name `vibamix-XXXX`.)

## How to use

1. **Wake a badge into config mode** (press its button) so it is connectable and acts as the gateway.
2. **Scan**, pick the `vibamix-XXXX` device, **Connect** (status turns green, MTU shown).
3. **Direct (GATT)** tab — talks straight to *that* badge: set the identity (name + table ID + LED),
   upload a render-only image, store/show the 20 text frames, upload to the 4 image slots (1-bit B/W
   or 2-bit grayscale, with a live dithered preview), and the Display command.
4. **Mesh** tab — injects mesh messages **through the connected badge's mesh-TX gateway char**,
   flooding to the group `0xC000` (all badges) or a unicast address. The mesh surface is small and
   ephemeral: heartbeat (one-shot or auto every 60 s), live LED, a render-only image, and draw-only
   text. **Stored content (name, table ID, stored frames, display selection) is GATT-only** — set it
   per badge over the Direct tab.

5. **Batch** tab — runs **one** GATT action (using the Direct-tab values) on every *checked* badge,
   one at a time (connect → do → disconnect).
6. **Bulk (XLSX)** tab — programs **full per-badge content** (identity image + name/id, text frames,
   image slots) from a spreadsheet. See **Bulk content programming (XLSX)** below.
7. **Flash** tab — bulk-flashes firmware to attached badges over **SWD** (probe-rs), independent of
   BLE. Flashes the bootloader + a *bootable* app to **every plugged-in probe at once** and assigns
   each a unique factory id. See **Flashing firmware (SWD)** below.

### Important: how laptop "mesh" works

A laptop has no mesh radio. The tool connects over GATT to a **config-mode** badge and writes a
vendor-model access payload to its **mesh-TX gateway** characteristic (`f0de000C`); that badge
re-originates it onto the mesh as a node, using the fleet's baked keys. (There is **no** SIG Mesh
GATT Proxy anymore — a badge is connectable only in config mode.) That badge is your **gateway**;
a broadcast only reaches **other badges that are also awake**. Use the **Auto heartbeat** toggle to
hold a fleet of (already-woken) badges awake. A heartbeat can **not** wake a sleeping badge — its
radio is off in deep sleep.

## Bulk content programming (XLSX)

The **Bulk (XLSX)** tab provisions a fleet's *flash content* from a spreadsheet — one **row per
badge**, applied in order to the **checked** devices (row 1 → first checked badge, etc.). It reuses
the per-badge GATT writes (identity / text frame / image slot), so put each target badge in config
mode and check it first.

At run start it **opens a connection to every checked badge at once and holds them with a 1 Hz
keepalive**, so badges late in the queue don't hit their config-mode timeout and sleep before they're
programmed; it then writes each badge's content in turn and disconnects them all at the end. (Holding
many simultaneous BLE connections depends on your laptop's Bluetooth controller — some support fewer
than ~20; badges that fail to connect are marked failed and skipped, the rest still program.)

1. **Save template…** writes a starter `.xlsx` with the recognized header row + an example.
2. Fill in one row per badge, **Load XLSX…**, review the summary + any warnings in the log, then
   **Run on checked**. Mismatched counts program the first `min(rows, checked)` and warn.

Columns are **named headers** (case-insensitive, any order — omit unoccupied frames). Recognized:

| Header | Meaning |
|--------|---------|
| `name` | identity display name (≤31 B) |
| `attendee_id` (aliases `attendee`, `table_id`) | identity attendee/table id (≤10 B) |
| `identity_image` | path to the identity background image |
| `identity_image_fmt` | `bw` or `gray2` (optional; default `gray2`) |
| `text_label_N` / `text_body_N` | text frame N (N = 1..20 → slot idx N-1) — the label/contents pair |
| `image_N` | path for image slot N (N = 1..4 → slot N-1) |
| `image_N_fmt` | `bw` or `gray2` for slot N (optional; default `gray2`) |
| `identity_led` / `identity_anim` | identity-frame LED color / animation (optional) |
| `text_led_N` / `text_anim_N` | text frame N LED color / animation (optional) |
| `image_led_N` / `image_anim_N` | image slot N LED color / animation (optional) |

Image cells are file paths resolved relative to the sheet's directory and are validated on **Load**:
if any referenced image is missing or can't be decoded, the load fails with an error listing every
bad image (fix them before running). Blank cells / absent columns are left unchanged on the badge.
Identity name + attendee id are written only when present.

**Per-frame LED:** a `*_led` cell is a color **name** (`red`, `orange`, `white`, `teal`, …) or
**hex** (`#ff8800`, `ff8800`, or 3-digit `#f80`); a `*_anim` cell is an animation name (`off`,
`solid`, `rainbow`, `wheel`, `breathe`, `comet`, `sparkle`, `rainbow sparkle`) or its 0–7 code. If a
frame has a color but no animation it defaults to **Solid**; an animation with no color runs on
black; a frame with neither stays **Off**. Identity LED is written together with the name (there is
no LED-only identity write), so it is applied only on rows that also set `name`/`attendee_id`.

## Flashing firmware (SWD)

The **Flash** tab folds in the core of the standalone `firmware/flashtool` — it programs a fleet of
badges over their on-module CMSIS-DAP probes — without that tool's USB-hub port mapping. It flashes
**every connected probe in parallel**; there is no per-port slot map.

Prerequisite: install **probe-rs** separately (`cargo install probe-rs-tools`, or a pre-built binary
on `PATH`).

1. **Build the images first** (the tab points at pre-built artifacts):
   ```sh
   cd firmware/vibamix_zephyr && bash scripts/build_slots.sh
   ```
   This produces `build/slotA.bin` (the **trailered**, cold-bootable app) and
   `build/bl/zephyr/zephyr.hex` (the bootloader) — the Flash tab's two default paths.
2. Plug in the badges (each XIAO exposes a CMSIS-DAP probe). They appear in the **Flash** tab's probe
   table (auto-refreshed); hit **Refresh probes** to rescan now.
3. Confirm the two image paths (override via **Browse**), leave **Write per-unit factory id** on, and
   press **Flash all**. Each probe flashes concurrently: bootloader → app (slot A, `0xE000`) →
   factory id → reset. Per-probe progress shows in the table; the board boots the app standalone.

   - **Full chip erase first** (checkbox, off by default): wipes *all* nonvolatile memory before
     reflashing — leftover mesh provisioning, settings/ZMS, and stored images/screens — for a
     factory-clean badge (the equivalent of `west flash --erase`). Incremental flashing only rewrites
     the sectors it touches, so use this when you want to clear stale on-device state. Slower, and it
     erases the bootloader too (which the same run re-flashes). The factory id is unaffected — it's
     re-derived from `serials.json` by probe serial.

The app **must** be the trailered `slotA.bin`, not a plain `zephyr.hex`: the direct-XIP bootloader
CRC-verifies the slot via its 32-byte `VIMG` trailer before booting, so an un-trailered app only runs
under an attached debugger.

**Factory ids** are unique 15-bit values (mesh unicast / config code / GAP name) assigned per probe
serial and persisted to `~/.badgectl/serials.json`. **Back that file up** — losing it restarts ids at
1 and risks duplicates on already-flashed badges.

## Notes

- **Keys are duplicated from the firmware** (`badgectl/keys.py` mirrors `mesh_keys.h`,
  `config_gatt.c`, `mesh_model.c`). If the firmware keys/opcodes/UUIDs change, update `keys.py`.
- **Sequence numbers** are persisted to `~/.badgectl/seq.json` per source address so the badge's
  replay protection isn't tripped across runs. Default mesh `Src` is `0x0001` (configurable; pick
  one that won't collide with a badge's FICR-derived unicast).
- Validate the mesh crypto without hardware: `poetry run python -m badgectl.mesh`
  (checks `k2`/`k4` against the Mesh Profile §8 sample vectors).

## Layout

| File | Role |
|------|------|
| `badgectl/keys.py` | Baked keys, UUIDs, opcodes (mirror of firmware). |
| `badgectl/mesh.py` | Bluetooth-Mesh proxy-client crypto + PDU/segmentation builder. |
| `badgectl/ble.py` | bleak scan + one connection (GATT framing + proxy write). |
| `badgectl/imageconv.py` | Pillow → 1-bit framebuffer / 2-bit grayscale packing. |
| `badgectl/bulkprog.py` | XLSX bulk-content parser → per-badge `RowSpec` (identity/text/image). |
| `badgectl/seqstore.py` | Persisted mesh sequence number. |
| `badgectl/probes.py` | Enumerate attached CMSIS-DAP probes via `probe-rs list`. |
| `badgectl/flash.py` | probe-rs flashing backend (bootloader + app + factory id). |
| `badgectl/serialreg.py` | Per-unit factory-id assignment + persistence. |
| `badgectl/flashconfig.py` | Flash constants + default artifact paths. |
| `badgectl/gui.py` | PyQt6 window (GATT + Mesh + Batch + Bulk + Flash panels, log console). |
