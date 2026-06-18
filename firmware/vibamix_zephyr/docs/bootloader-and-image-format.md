# Vibamix — Bootloader, Image Format & OTA File Structure

This document describes the **custom direct‑XIP A/B bootloader**, the **app image layout** (the
32‑byte CRC trailer), the **`bl_state` boot‑metadata log**, and the **`.ota` bundle file format**.
It is the authoritative reference for byte layouts; the GATT/BLE *wire* framing for streaming an
update is in [ble-config-api.md §10](ble-config-api.md). Source of truth in code:

- [common/vbx_img.h](../../common/vbx_img.h) — image trailer struct + verify
- [common/bl_state.h](../../common/bl_state.h) / [.c](../../common/bl_state.c) — per‑slot boot metadata log
- [bootloader/src/main.c](../../bootloader/src/main.c) — the first‑stage bootloader
- [src/ota.c](../src/ota.c) / [src/slots.h](../src/slots.h) — the app‑side OTA receiver
- [scripts/vbx_trailer.py](../scripts/vbx_trailer.py) — appends the CRC trailer
- [scripts/pack_ota.py](../scripts/pack_ota.py) — bundles both slots into a `.ota`

---

## 1. Why this design (no MCUboot)

The badge uses a **custom first‑stage bootloader** instead of MCUboot/sysbuild. It is a
**direct‑XIP A/B** scheme: there are two equal‑size app slots, the app is built **twice** (once
linked at each slot's flash offset), and the bootloader **chain‑loads the chosen slot in place** —
it never copies/swaps images. RRAM on the nRF54L15 is memory‑mapped (XIP) at its flash offset, so
both the bootloader and the app read slot images and trailers through plain pointers.

Properties this gives us:

- **Power‑fail‑safe OTA.** A new image is written into the **inactive** slot; the running slot is
  never touched, so a power loss mid‑update just leaves the old image bootable.
- **Auto‑revert.** A freshly‑OTA'd image boots as a **trial**; if it doesn't **confirm** (reach a
  healthy state) before a watchdog fires, the bootloader rolls back to the previous confirmed slot.
- **Tiny + inspectable.** No image signing/encryption; integrity is a CRC‑32. (Security tradeoff is
  intentional for a badge demo — see the mesh‑keys note in [AGENTS.md](../AGENTS.md).)

---

## 2. Flash map

`cpuapp_rram`, 1428 KB total. Defined in the board DTS
([vibamix_xiao_nrf54l15_cpuapp.dts](../boards/pants_for_birds/vibamix_xiao/vibamix_xiao_nrf54l15_cpuapp.dts), `partitions` node):

| Partition (DTS label) | Node | Offset | Size | Purpose |
|-----------------------|------|--------|------|---------|
| `bootloader` | `bootloader_partition` | `0x0` | 48 KB | first‑stage BL (flashed once over SWD) |
| `bl_state` | `bl_state_partition` | `0xC000` | 8 KB | per‑slot boot metadata append log (2 × 4 KB sectors) |
| **`image-a` (slot A)** | `slot_a_partition` | `0xE000` | 512 KB | app image, A build |
| **`image-b` (slot B)** | `slot_b_partition` | `0x8E000` | 512 KB | app image, B build |
| `images` | `images_partition` | `0x10E000` | 64 KB | 4 × 16 KB raw badge‑image slots (unrelated to OTA) |
| `storage` | `storage_partition` | `0x11E000` | 284 KB | settings (ZMS); `chosen { zephyr,settings-partition }` |

The two app slots are **equal size** (512 KB) — required so an image linked for A is the same size
budget as B. The actual app image is a small fraction of a slot.

> Changing this layout requires a full `west flash --erase`; `images` + `storage` move relative to
> the old MCUboot layout, so the first flash of this scheme wipes stored badge images + ZMS once
> (the mesh re‑provisions from baked keys).

---

## 3. App image structure

Each slot holds a **plain single‑image Zephyr build** linked at the slot base, with a **32‑byte CRC
trailer appended after the image bytes**:

```
slot base (0xE000 for A / 0x8E000 for B)
┌───────────────────────────────────────────────┬──────────────────────┐
│  app image  (vector table first, .text, .rodata…)  padded to 16 B     │  32-byte VIMG trailer │
│  ── image_len bytes ──────────────────────────────────────────────►   │  at base + image_len  │
└───────────────────────────────────────────────┴──────────────────────┘
```

- The **vector table sits at the slot base** (`CONFIG_ROM_START_OFFSET=0`); the bootloader reads
  `msp`/`reset` from there and jumps. The slot is selected at build time by the DTS
  `zephyr,code-partition` (`CONFIG_USE_DT_CODE_PARTITION`) — slot A is the default DTS; slot B is
  built with `slotb.overlay` repointing the code‑partition (see [build_slots.sh](../scripts/build_slots.sh)).
- The trailer is a **suffix, not a header** — the image keeps its natural load address. The image is
  zero‑padded to a 16‑byte boundary first, so the trailer is 16‑aligned and `image_len` is a
  multiple of 16.

### 3.1 The CRC trailer (`struct vbx_img_trailer`, 32 bytes)

From [common/vbx_img.h](../../common/vbx_img.h). All fields little‑endian:

| Offset | Size | Field | Value |
|--------|------|-------|-------|
| 0  | u32 | `magic` | `0x58474D49` — ASCII **"IMGX"** (`VBX_IMG_MAGIC`) |
| 4  | u32 | `image_len` | byte count of the (padded) image preceding the trailer |
| 8  | u32 | `crc32` | CRC‑32/IEEE over the first `image_len` bytes |
| 12 | u32 | `version` | monotonic build/seq number (epoch seconds at build time) |
| 16 | u16 | `hdr_version` | trailer format version (= 1) |
| 18 | u16 | `reserved` | 0 |
| 20 | 12 B | `pad` | zero → rounds the struct to 32 bytes |

**Verification** (`vbx_img_verify`): the trailer is located at `base + image_len`; it passes if
`magic == "IMGX"` **and** the trailer's `image_len` matches the expected length **and**
`crc32_ieee(base, image_len)` equals the stored `crc32`. The same `version` is what the bootloader
compares to pick the newer of two valid slots.

> **CRC‑32 details.** Standard CRC‑32/IEEE (zlib/PNG): poly `0xEDB88320`, init `0xFFFFFFFF`,
> reflected in/out, final XOR `0xFFFFFFFF`. Host side uses `zlib.crc32`; firmware uses `crc32_ieee`.

### 3.2 How the trailer is produced

- **Build time:** each slot build's CMake `POST_BUILD` runs
  [scripts/vbx_trailer.py](../scripts/vbx_trailer.py) on `zephyr.bin` → `build/slotA.bin` /
  `build/slotB.bin` (raw image + trailer). The `--version` is the shared epoch‑seconds value.
- **OTA receive time:** the image arrives **already trailered** (the host streams `slotX.bin`
  verbatim); the receiver writes those exact bytes and re‑verifies in place. So the trailer format
  is identical whether an image is SWD‑flashed or OTA'd.

---

## 4. `bl_state` — per‑slot boot metadata

The bootloader can't keep its decision state in an app slot (that's the thing being replaced), so
per‑slot status lives in the dedicated **`bl_state`** partition as a **32‑byte append log**
([common/bl_state.h](../../common/bl_state.h) / [.c](../../common/bl_state.c)). Writes append a new
record; the **latest record with a valid CRC‑8 and highest `seq`** wins. A torn write fails CRC‑8
and is ignored, so the previous record survives. When the log fills, it erases and restarts from the
current snapshot.

**Record** (`struct bl_state_rec`, 32 bytes, little‑endian):

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | u32 | `magic` | `0x53584256` — ASCII **"VBXS"** |
| 4 | u16 | `seq` | monotonic; highest = authoritative |
| 6 | u8  | `crc8` | CRC‑8/CCITT over the record with this field zeroed |
| 7 | u8  | `rsvd` | 0 |
| 8 | 12 B | `slot[0]` | slot A metadata (below) |
| 20 | 12 B | `slot[1]` | slot B metadata |

**Per‑slot** (`struct bl_slot_meta`, 12 bytes):

| Size | Field | Meaning |
|------|-------|---------|
| u32 | `image_len` | image size — locates the trailer; `0` = no image |
| u32 | `version` | build version (mirrors the trailer) |
| u8  | `valid` | 1 = an OTA wrote + CRC‑verified this slot |
| u8  | `confirmed` | 1 = the app booted **healthily** from this slot |
| u8  | `attempts` | trial boots since the last confirm (cap `BL_MAX_ATTEMPTS = 3`) |
| u8  | `rsvd` | 0 |

App‑side helpers (read‑modify‑append): `bl_state_set_pending(slot, version, image_len)` (OTA just
wrote a slot), `bl_state_confirm(slot)` (healthy boot), `bl_state_inc_attempts(slot)` (bump trial
count — the bootloader calls this before a trial jump).

---

## 5. The boot decision (bootloader)

[bootloader/src/main.c](../../bootloader/src/main.c), `main()`:

1. **Debugger attached?** (`DHCSR.C_DEBUGEN`) → boot **slot A** unconditionally. This is the dev/
   flash‑debug path: a debugger `load`s an unsigned ELF into slot A and we run it as‑is, so the F5
   load/reset loop works without trailers or `bl_state`.
2. **Normal pick** from `bl_state`: among slots that are `valid`, whose **trailer CRC live‑verifies**,
   and that are `confirmed` **or** still have a trial attempt left (`attempts < 3`), choose the
   **highest `version`**. A slot that is unconfirmed and out of attempts is skipped (→ revert to the
   other confirmed slot).
3. **Fallback — fresh SWD flash:** if `bl_state` has nothing usable and slot A was never recorded,
   **scan slot A** for a self‑consistent trailer and trial‑boot it (recording it pending first).
4. **Last resort:** scan **both** slots for any CRC‑valid image and trial‑boot the highest‑version
   one — a slow retry loop beats a dead board (rescues the "good new image, no trailered revert
   target" case). If still nothing → draw `HALT NO BOOTABLE IMG` and spin.

**Trial / confirm / revert (watchdog):**

- A **trial** boot (an unconfirmed image) **increments `attempts` before jumping** and **arms
  `wdt31`** with a **30 s** window (`WDT_FLAG_RESET_SOC`, paused in sleep + when halted by a
  debugger). The watchdog keeps running across the jump.
- The app, once it boots far enough to be healthy, calls `ota_confirm_on_boot()` (sets `confirmed`,
  clears the trial) and thereafter **feeds the watchdog**. A confirmed boot "sticks."
- If a trial image **crashes, resets, or hangs** before confirming, the watchdog resets the SoC; on
  the next boot that slot has used an attempt, so the bootloader **reverts** to the other confirmed
  slot. After `BL_MAX_ATTEMPTS` (3) failed trials the unconfirmed image is dropped.

**On‑screen diagnostics.** On a **trial**, a **revert**, or a **halt**, the bootloader draws an
ePaper diagnostic screen *before* chain‑loading (so the refresh doesn't eat the trial's watchdog
budget): the decision (`TRIAL SLOT B` / `REVERT TO A` / `HALT NO BOOTABLE IMG`) and, per slot,
`VALID` / live `CRC OK/FAIL` / `VER` / `LEN` / `ATT` / `CONF`. A normal confirmed boot draws nothing
(no slowdown). See [bootloader/src/diag.c](../../bootloader/src/diag.c).

---

## 6. App‑side OTA receiver

[src/ota.c](../src/ota.c) + [src/slots.h](../src/slots.h). The running image knows which slot it is
(`MY_SLOT`, derived at compile time from `CONFIG_FLASH_LOAD_OFFSET`) and therefore which slot is the
target (`OTHER_SLOT` / `OTHER_SLOT_FA_ID`).

Flow, driven by the GATT OTA characteristic ([config_gatt.c](../src/config_gatt.c) `f0de0009`, wire
framing in [ble-config-api.md §10](ble-config-api.md)):

1. `ota_begin(total_size)` — open the **inactive** slot's flash area, bounds‑check `total_size`
   against the slot, and **erase** enough 4 KB blocks. `total_size` is the image **including** its
   32‑byte trailer.
2. `ota_write(offset, data, len)` — append **in order** (`offset` must equal the running count).
   Writes are committed in **16‑byte write‑blocks** (nRF54L15 RRAM write granularity); a small
   accumulator handles unaligned chunk boundaries.
3. `ota_finish(crc)` — flush a final zero‑padded block, then **verify the image in place**
   (`vbx_img_verify` over `total_size − 32` bytes) **and** cross‑check the host‑supplied `crc`
   against the trailer's. On success: `bl_state_set_pending(OTHER_SLOT, version, image_len)` and
   schedule a **cold reboot ~1.2 s later**. The bootloader then trial‑boots the new slot.
4. `ota_confirm_on_boot()` — called early in `main()`; confirms `MY_SLOT` so a healthy boot of a
   freshly‑trialed image makes it permanent.

The active slot is never written, and any failure (`size mismatch`, bad trailer, CRC mismatch)
aborts without marking the slot pending — the badge keeps running the old image.

---

## 7. The `.ota` bundle file

Rather than ship two loose `slotA.bin` / `slotB.bin` and pick by hand, the build packs **both** into
one **`.ota`** container ([scripts/pack_ota.py](../scripts/pack_ota.py) → `build/vibamix.ota`). The
host loads the single file, reads the badge's running/inactive slot over BLE (OTA‑status
`f0de000A`), finds the **inactive** slot's directory entry, and streams just that slot's image — so
the app can never send the wrong‑slot image.

**Container layout** (little‑endian):

```
┌──────────────── 16-byte header ────────────────┐
│ 0   "VOTA"  (4)         magic                    │
│ 4   u16 format_version  (= 1)                     │
│ 6   u16 slot_count      (= 2)                     │
│ 8   u32 app_version     (shared by both slots)    │
│ 12  u32 reserved (0)                              │
├──────────── directory: slot_count × 12 B ────────┤
│ per entry:  u8 slot (0=A,1=B) | u8 pad[3]         │
│             u32 offset  (absolute, into the file) │
│             u32 length  (image incl. 32B trailer) │
├──────────────── slot payloads ───────────────────┤
│ each slot's slotX.bin (raw image + trailer) at    │
│ its directory `offset`                            │
└───────────────────────────────────────────────────┘
```

| Field | Offset | Size | Notes |
|-------|--------|------|-------|
| `magic` | 0 | 4 | `"VOTA"` = `0x56 0x4F 0x54 0x41` |
| `format_version` | 4 | u16 | currently `1` |
| `slot_count` | 6 | u16 | `2` (A and B) |
| `app_version` | 8 | u32 | same value as both slots' trailer `version` |
| `reserved` | 12 | u32 | `0` |
| directory[] | 16 | 12 × `slot_count` | one entry per slot (below) |
| payloads | 16 + 12·n | — | each `slotX.bin` at its entry's `offset` |

**Directory entry** (12 bytes): `u8 slot`, `u8 pad[3]`, `u32 offset` (absolute byte offset of this
slot's image within the file), `u32 length` (image length **including** the 32‑byte trailer). The
payload bytes are *exactly* a `slotX.bin`, so the OTA END `crc32` is computed over `length − 32`.

Host extraction (pseudocode): parse the header → read OTA‑status to learn the **inactive** slot →
find that directory entry → stream `file[offset : offset + length]` through the GATT OTA framing.

---

## 8. End‑to‑end

```sh
# 1. Build BOTH slots + trailers + the bundle (from firmware/vibamix_zephyr/):
bash scripts/build_slots.sh
#    -> build/slotA.bin, build/slotB.bin  (raw image + 32-byte VIMG trailer)
#    -> build/vibamix.ota                 (both slots, directory header)

# 2. Flash the first-stage bootloader ONCE over SWD (at 0x0):
west build -p always -b vibamix_xiao/nrf54l15/cpuapp --no-sysbuild -d build/bl \
  bootloader -- -DBOARD_ROOT=vibamix_zephyr
west flash --erase -d build/bl

# 3. Seed a first app image over SWD (a trailered slot A, so revert has a target):
probe-rs download --binary-format bin --base-address 0xE000 build/slotA.bin

# 4. Thereafter, update over BLE: badgectl reads OTA-status, picks the inactive
#    slot from vibamix.ota, streams it; the badge trials + confirms (or reverts).
```

See [firmware/README.md](../../README.md) for the full toolchain/runner details and the three
app‑run paths. The **GATT OTA wire framing** (START/DATA/END opcodes, chunk sizes, status
characteristic) is in [ble-config-api.md §10](ble-config-api.md).

### Two OTA gotchas (both about the *other* slot, not the new image)

- **Rollback needs a trailered image in the other slot.** Auto‑revert can only fall back to a slot
  whose CRC trailer verifies. An app `load`ed by a debugger (F5 / `gdb load`) has **no trailer** and
  is *not* a valid revert target — SWD‑flash a trailered `slotA.bin` (step 3) to have a real
  fallback.
- **Detach the debugger before triggering OTA.** While a probe is attached the bootloader takes its
  dev path and boots **slot A** regardless of `bl_state`, so a freshly‑OTA'd slot B silently won't
  run, and `sys_reboot()` under a halted probe may not reset. Close the debug session first.
