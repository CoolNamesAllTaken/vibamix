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

1. **Wake a badge** (press its button) so it is connectable and acts as the gateway.
2. **Scan**, pick the `vibamix-XXXX` device, **Connect** (status turns green, MTU shown).
3. **Direct (GATT)** tab — talks straight to *that* badge: set name, upload a render-only
   image, store/show the 20 text screens, upload to the 4 image slots (1-bit B/W or 2-bit
   grayscale, with a live dithered preview), and the Display command.
4. **Mesh** tab — injects mesh messages **through the connected badge's proxy**, flooding to
   the group `0xC000` (all badges) or a unicast address: heartbeat (one-shot or auto every
   60 s), name, fun fact, LED color, text screen, display. Long names/bodies auto-segment.

### Important: how laptop "mesh" works

A laptop has no mesh radio. The tool connects over GATT to **one awake badge's Mesh Proxy**
and injects encrypted PDUs as a mesh node, using the fleet's baked keys. That badge is your
**gateway**; a broadcast only reaches **other badges that are also awake**. Use the
**Auto heartbeat** toggle to hold a fleet of (already-woken) badges awake. A heartbeat can
**not** wake a sleeping badge — its radio is off in deep sleep.

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
| `badgectl/seqstore.py` | Persisted mesh sequence number. |
| `badgectl/gui.py` | PyQt6 window (GATT + Mesh panels, log console). |
