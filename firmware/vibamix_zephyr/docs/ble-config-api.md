# Vibamix Badge — BLE Configuration API

This document specifies the Bluetooth Low Energy interface a **configuration app** uses to set up a
vibamix badge: connect to a specific badge, set the attendee **name**, upload hand‑drawn **badge
images** (B/W or grayscale) into **4 storage slots**, store up to **20 text screens**, and command
which stored screen the badge displays. It also covers the event‑wide **mesh** control path
(broadcast display/screen commands + the keep‑awake heartbeat).

It is a complete handoff spec — you should not need the firmware source to implement the app.

---

## 1. How it works (overview)

- The badge spends most of its life powered off. **The attendee presses the button** on the badge;
  it wakes into **config mode**, shows a unique 4‑character **code** and a **QR code**, and becomes
  connectable for ~3 minutes (the window resets on activity).
- The QR encodes a URL like `https://<your-host>/?id=1A2F`. Your web app reads the `id` query
  param — it is the badge's code, and the badge advertises with the BLE name **`vibamix-<CODE>`**
  (e.g. `vibamix-1A2F`), so you can guide the user to pick the right device.
- The app connects over **GATT** to a custom **Config Service** and writes two things:
  - a **Name** characteristic (UTF‑8 string), and/or
  - an **Image** characteristic (a chunked upload of a 1‑bit framebuffer).
- This is a **direct GATT connection** to one badge. It is independent of the badge mesh network;
  you do not need to know anything about the mesh.

### Platform support (important)
The app uses the **Web Bluetooth API** (`navigator.bluetooth`).
- ✅ Chrome / Edge on **Android, Windows, macOS, Linux, ChromeOS**.
- ❌ **iOS/iPadOS Safari does not support Web Bluetooth.** iPhone/iPad users must open the page in
  the **Bluefy** browser app (free), which provides Web Bluetooth. Detect iOS and show that hint.
- Web Bluetooth requires the page to be served over **HTTPS** (or `localhost`) and
  `requestDevice()` must be called from a **user gesture** (e.g. a button click).

---

## 2. Discovery & connection

1. Read `id` from the page URL (the QR put it there). Keep it to match the device the user picks.
2. From a click handler, request the device, filtering by name prefix and declaring the custom
   service so you may use it after connecting:

```js
const SVC  = 'f0de0001-4b1c-4e2a-9a11-a1b2c3d4e5f6';
const IMG  = 'f0de0002-4b1c-4e2a-9a11-a1b2c3d4e5f6';
const NAME = 'f0de0003-4b1c-4e2a-9a11-a1b2c3d4e5f6';

const device = await navigator.bluetooth.requestDevice({
  filters: [{ namePrefix: 'vibamix-' }],   // or { name: `vibamix-${id}` } if you have the id
  optionalServices: [SVC],
});

const server  = await device.gatt.connect();
const service  = await server.getPrimaryService(SVC);
const imageChar = await service.getCharacteristic(IMG);
const nameChar  = await service.getCharacteristic(NAME);
```

Notes:
- The badge must be **in config mode** (button pressed) to be connectable. If `requestDevice`
  shows nothing, tell the user to press the badge button again.
- The device may also appear by its mesh service; always select by the `vibamix-` **name**.
- There is no pairing/bonding requirement — writes are unauthenticated. The browser may still show
  its own device-chooser prompt (that is normal and required).

---

## 3. GATT service & characteristics

**Config Service** — UUID `f0de0001-4b1c-4e2a-9a11-a1b2c3d4e5f6`

| Characteristic | UUID | Properties | Payload |
|----------------|------|-----------|---------|
| **Name**  | `f0de0003-…` | Write | UTF‑8 string, **≤ 31 bytes**. Sets the attendee name and redraws the identity screen. |
| **Image** | `f0de0002-…` | Write, Write‑Without‑Response | Render-only 1‑bit image upload (see §5). Does **not** persist to a slot. |
| **Screen** | `f0de0004-…` | Write | Store one of **20 text screens** (header + body), framed (see §7). |
| **Image‑slot** | `f0de0005-…` | Write, Write‑Without‑Response | Upload an image into one of **4 stored slots**, 1‑bit B/W or 2‑bit grayscale (see §8). |
| **Display** | `f0de0006-…` | Write | Show a stored screen: `u8 kind, u8 idx` (see §9). |
| **Attendee‑ID** | `f0de0007-…` | Write | UTF‑8 string, **≤ 10 bytes** — the table/seat ID shown on the identity screen (see §9.1). |
| **Frame‑LED** | `f0de0008-…` | Write | Per‑frame LED animation + color: `u8 kind, u8 idx, u8 anim, u8 r, u8 g, u8 b` (see §9.2). |
| **OTA** | `f0de0009-…` | Write, Write‑Without‑Response | Firmware update: stream the trailered image for the inactive direct‑XIP slot, then reboot (see §10). |
| **OTA‑status** | `f0de000A-…` | Read | `u8 active_slot, u8 inactive_slot, u32 active_version` — tells the host which slot image to send (see §10). |

(All UUIDs share the base `…-4b1c-4e2a-9a11-a1b2c3d4e5f6`.) All multi‑byte integers in payloads are
**little‑endian**.

There is **no notify/indicate** characteristic — the badge does not send a status back. Success is
visible on the e‑paper screen (and the badge logs to its UART). See §7 for the implication.

---

## 4. Setting the name

Write a UTF‑8 string (no length prefix, no terminator) to the **Name** characteristic:

```js
await nameChar.writeValueWithResponse(new TextEncoder().encode('Ada Lovelace'));
```

- Max **31 bytes** stored; extra bytes are dropped by the firmware.
- Writing the name **redraws the text identity screen**, which **replaces any uploaded image**.
  So if you set both, **send the name first and the image last** to leave the image on screen.
- The name persists across power cycles.

---

## 5. Uploading an image

The image is sent as a short command stream to the **Image** characteristic. Each GATT write is one
framed message: a 1‑byte opcode followed by a little‑endian payload.

| Opcode | Name | Payload (after the opcode byte) | Meaning |
|-------:|------|---------------------------------|---------|
| `0x01` | **START** | `u16 size`, `u16 width`, `u16 height` | Begin a transfer. `size` = total image bytes = **5808**, `width` = **176**, `height` = **264**. Resets/zero‑fills the buffer. |
| `0x02` | **DATA**  | `u16 offset`, then `N` image bytes | Write `N` bytes at `offset` into the image buffer. |
| `0x03` | **END**   | `u32 crc32` | Finish. Firmware verifies all bytes arrived and the CRC matches, then renders. |

Rules enforced by the firmware:
- `size` must be **1…5808**. Use **5808** (a full frame). The image **must** be exactly 5808 bytes —
  any bytes you don't send remain `0x00` = **black**.
- Each DATA chunk must satisfy `offset + N ≤ size`. Send chunks in increasing offset; the whole
  buffer must be covered.
- **END** is rejected (image not rendered) if fewer than `size` bytes were received **or** the CRC
  does not match — so a single dropped chunk fails the whole upload.

### CRC
`crc32` is the standard **CRC‑32/IEEE** (the zlib/PNG CRC): polynomial `0xEDB88320`, init
`0xFFFFFFFF`, input/result reflected, final XOR `0xFFFFFFFF`. Compute it over the full **5808‑byte**
buffer. Send it little‑endian in END.

### Chunk size / MTU
The badge negotiates a large ATT MTU (up to 247), but browsers/OSes vary (iOS ≈ 185). A **DATA
payload of 180 image bytes** (so a 183‑byte characteristic value: `1 + 2 + 180`) is safe
everywhere; you can go larger (up to ~240) on desktop. 5808 ÷ 180 ≈ **33 writes**.

### Reliability
Use **`writeValueWithResponse`** for the DATA chunks — it waits for each chunk to be acknowledged,
which is the simplest way to guarantee delivery (a dropped chunk would fail the CRC). If you use
`writeValueWithoutResponse` for speed, you must pace writes and accept that any loss means the whole
image is rejected (retry the upload).

### Upload sequence (JS)

```js
const W = 176, H = 264, SIZE = 5808;       // fixed for this panel
const CHUNK = 180;                          // image bytes per DATA write

async function uploadImage(imageChar, buf /* Uint8Array(5808) */) {
  // START
  const start = new DataView(new ArrayBuffer(7));
  start.setUint8(0, 0x01);
  start.setUint16(1, SIZE, true);
  start.setUint16(3, W, true);
  start.setUint16(5, H, true);
  await imageChar.writeValueWithResponse(start.buffer);

  // DATA
  for (let off = 0; off < SIZE; off += CHUNK) {
    const n = Math.min(CHUNK, SIZE - off);
    const pkt = new Uint8Array(3 + n);
    pkt[0] = 0x02;
    pkt[1] = off & 0xff;
    pkt[2] = (off >> 8) & 0xff;
    pkt.set(buf.subarray(off, off + n), 3);
    await imageChar.writeValueWithResponse(pkt);
  }

  // END
  const end = new DataView(new ArrayBuffer(5));
  end.setUint8(0, 0x03);
  end.setUint32(1, crc32(buf) >>> 0, true);
  await imageChar.writeValueWithResponse(end.buffer);
}
```

The e‑paper full refresh takes ~2 s after END; the new image then stays on screen (the panel is
bistable) until the badge is reconfigured.

---

## 6. The image buffer format (how to build the 5808 bytes)

The badge screen is **176 × 264, 1 bit per pixel**. The 5808‑byte buffer is the panel's native
framebuffer: **264 rows × 22 bytes/row**, each byte = 8 horizontal pixels, **MSB = leftmost**.
Bit value **`1` = white, `0` = black**.

The badge is meant to be read in **landscape: 264 wide × 176 tall** (the name/QR screens use that
orientation). So author your drawing on a **264 × 176** canvas and pack it with the mapping below,
which matches exactly how the firmware renders text/QR (a 270° rotation of the canvas onto the
portrait panel).

For a drawing pixel at landscape coordinates `(dx, dy)` — `dx` in `0…263` (left→right),
`dy` in `0…175` (top→bottom):

```
xmem      = dy
ymem      = 263 - dx
byteIndex = ymem * 22 + (xmem >> 3)
bitMask   = 0x80 >> (xmem & 7)
```

Initialize the buffer to all‑white (`0xFF`) and clear the bit for each black pixel.

```js
// blackAt(dx, dy): true where the drawing is black. dx∈[0,264), dy∈[0,176).
function packFramebuffer(blackAt) {
  const buf = new Uint8Array(5808).fill(0xff);    // white
  for (let dx = 0; dx < 264; dx++) {
    for (let dy = 0; dy < 176; dy++) {
      if (blackAt(dx, dy)) {
        const xmem = dy, ymem = 263 - dx;
        buf[ymem * 22 + (xmem >> 3)] &= ~(0x80 >> (xmem & 7));
      }
    }
  }
  return buf;
}
```

Typical pipeline: draw on a 264×176 `<canvas>` → `getImageData` → threshold each pixel to
black/white (e.g. luminance < 128 → black) → `packFramebuffer`.

> **Verify orientation on real hardware first.** The mapping above is taken directly from the
> firmware's display path, but the cheapest way to be 100% sure is to upload an **asymmetric test
> pattern** (e.g. a filled top‑left quadrant, or the letter "F") and confirm it appears upright and
> not mirrored. If it's rotated/mirrored, the only thing to change is this packing function — the
> building blocks are: the panel is physically 176×264 portrait; the firmware applies a 270°
> rotation; and the row order is flipped vertically. Adjust `xmem`/`ymem` accordingly.

---

## 7. Text screens (stored content)

The badge stores **20 text screens**, indexed `0…19`, each a **header** (≤ 47 bytes) and a
**body** (≤ 1023 bytes, word‑wrapped on the badge). Screens persist across power cycles. Set one by
writing a framed stream to the **Screen** characteristic (`f0de0004`):

| Opcode | Payload | Meaning |
|-------:|---------|---------|
| `0x01` START | `u8 idx`, `u8 hlen`, `hlen` header bytes | Begin screen `idx`; carries the full header. |
| `0x02` DATA  | `u16 offset`, body bytes | Body bytes at `offset` (chunk for bodies > one MTU). |
| `0x03` END   | — | Commit (store header + accumulated body). |

The body is reassembled by offset (like an image), so cover `0…blen‑1`; short bodies can be a single
DATA at offset 0. Setting a screen only **stores** it — use **Display** (§9) to show it.

---

## 8. Image slots (4 stored images, B/W or grayscale)

The badge stores **4 full‑screen image slots**, indexed `0…3`. Each slot is either:
- **1‑bit B/W** (`format = 1`) — the panel‑native 5808‑byte framebuffer (§6 packing). Displayed by a
  direct blit (crisp; pre‑dither on the host for best art).
- **2‑bit grayscale** (`format = 2`) — a 11,616‑byte image the badge **dithers to B/W** on display
  (the panel is physically 1‑bit, so grayscale is approximated with an ordered dither).

Upload to the **Image‑slot** characteristic (`f0de0005`), same framing as §5 but START carries the
slot and format:

| Opcode | Payload | Meaning |
|-------:|---------|---------|
| `0x01` START | `u8 slot`, `u8 format`, `u16 size`, `u16 width`, `u16 height` | Begin upload to `slot` (0–3). B/W: `size=5808`, `width=176`, `height=264`. Gray: `size=11616`, **`width=264`, `height=176`** (the landscape packing stride — the dither reads `pixel_index = dy*width + dx`). |
| `0x02` DATA  | `u16 offset`, image bytes | As §5. |
| `0x03` END   | `u32 crc32` | CRC‑32/IEEE over the whole image; on success the slot is stored **and** displayed. |

**2‑bit packing.** Author on the same 264 × 176 landscape canvas as §6. Pack **4 pixels per byte,
MSB‑first, row‑major**: `pixel_index = dy*264 + dx`; the 2‑bit level lives in bits
`[7:6],[5:4],[3:2],[1:0]` of byte `pixel_index >> 2`. Level **0 = black … 3 = white**. Total
= 264·176·2/8 = **11,616 bytes**.

> Images are **local‑GATT only** — they are never sent over the mesh (too large to flood).

---

## 9. Displaying a stored screen

Write 2 bytes to the **Display** characteristic (`f0de0006`): `u8 kind, u8 idx`.
- `kind = 0` → **text screen** `idx` (0–19). Renders its header + wrapped body.
- `kind = 1` → **image slot** `idx` (0–3). Blits (B/W) or dithers (grayscale) the stored image.

The selection persists, and the rendered screen takes over the panel (the config countdown stops
repainting). Displaying an empty slot/screen is a no‑op.

### 9.1 Attendee / table ID

Write a UTF‑8 string (no prefix/terminator) to the **Attendee‑ID** characteristic (`f0de0007`):

```js
await attndChar.writeValueWithResponse(new TextEncoder().encode('12'));   // e.g. table 12
```

- Stored **≤ 10 bytes** (extra dropped); persists across power cycles.
- Shown on the **identity screen** as `Table <id>` (below the name). Writing it redraws identity.

### 9.2 Per‑frame LED animation + color

Each frame (the 20 text screens and the 4 image slots) can carry its own LED animation + color,
shown on the 4 badge LEDs **while that frame is the displayed one**. Write 6 bytes to the
**Frame‑LED** characteristic (`f0de0008`): `u8 kind, u8 idx, u8 anim, u8 r, u8 g, u8 b`.

- `kind` = `0` text screen (`idx` 0–19) or `1` image slot (`idx` 0–3) — same as Display.
- `anim` codes: `0` Off/*no override*, `1` Solid, `2` Rainbow, `3` Wheel, `4` Breathe, `5` Comet,
  `6` Sparkle. Solid/Breathe/Comet/Sparkle use `r,g,b`; Rainbow/Wheel ignore it (firmware caps
  brightness).
- Persists per frame; applied when that frame is displayed (live if it's the current frame). `anim=0`
  means "no override" (fall back to the badge default), not a forced blackout. LEDs run during the
  badge's brief awake window, not while a phone is connected.

### 9.3 Connected indicator

While a phone is connected over this service the badge shows a **"Connected"** screen and **does not
time out** — it stays in config until the link drops. After disconnect it shows the **identity
screen with a countdown bar** (time to sleep, reset by the mesh heartbeat); when that elapses it
redraws a clean identity screen and sleeps.

---

## 10. Firmware update (OTA)

The badge uses a **custom direct‑XIP A/B bootloader** (no MCUboot). There are two app slots, A and
B; the app is built **twice**, linked at each slot's offset (`scripts/build_slots.sh` →
`slotA.bin` / `slotB.bin`). Each `slotX.bin` is the raw image with a **32‑byte CRC trailer**
appended (`scripts/vbx_trailer.py`). OTA streams the image for the **currently inactive** slot into
that slot; the first‑stage bootloader CRC‑verifies it and chain‑loads it on reboot. A **watchdog +
attempt counter** auto‑revert to the previous slot if the new image fails to boot healthily.

### The `.ota` bundle (ship one file)

Rather than ship two loose `slotA.bin` / `slotB.bin` and choose between them by hand, the build
packs **both** into one **`.ota`** bundle (`scripts/pack_ota.py` → `build/vibamix.ota`). The host
loads the single bundle, reads the badge's running/inactive slot over BLE (OTA‑status, below), and
streams the matching slot's image from the bundle — so the app only ever deals with one artifact and
can't send the wrong slot.

Container layout (little‑endian):

| Offset | Size | Field |
|--------|------|-------|
| 0  | 4   | magic `"VOTA"` (`0x56 0x4F 0x54 0x41`) |
| 4  | u16 | format version (= 1) |
| 6  | u16 | slot count (= 2) |
| 8  | u32 | app version (shared by both slots) |
| 12 | u32 | reserved (0) |
| 16 | 12 × slot_count | directory entries (below) |
| …  | …   | each slot's image payload at its `offset` |

Each 12‑byte directory entry: `u8 slot` (0 = A, 1 = B), `u8[3]` pad, `u32 offset` (absolute byte
offset of this slot's image in the file), `u32 length` (image length **including** its 32‑byte CRC
trailer). To flash: parse the header, read OTA‑status to get the **inactive** slot, find that slot's
directory entry, and stream `file[offset : offset+length]` as the OTA payload (below). The image
bytes are exactly a `slotX.bin` (raw image + 32‑byte trailer) — the END `crc32` is still computed
over `length − 32` bytes.

**Which slot to send:** read the **OTA‑status** characteristic `f0de000A-…` first (read‑only):

| Bytes | Meaning |
|-------|---------|
| `[0] u8` | active slot (0 = A, 1 = B) — the slot currently running |
| `[1] u8` | inactive slot — **send this slot's image** |
| `[2..5] u32` | active image version (little‑endian; 0 if the running image was flashed over SWD, not OTA'd) |

Send `slotA.bin` if the inactive slot is 0, `slotB.bin` if it is 1 — or, with the `.ota` bundle,
the payload of the matching directory entry. Sending the wrong slot's image (linked for the other
offset) will fail to boot and be reverted.

**Characteristic:** OTA `f0de0009-…` (Write / Write‑Without‑Response). Framed like the image upload
but with **u32** size/offset (the image far exceeds 64 KB). All fields little‑endian:

| Op | Bytes | Meaning |
|----|-------|---------|
| START `0x01` | `u8 op` `u32 total_size` | Begin; erases + opens the inactive slot, shows "Updating firmware". `total_size` includes the 32‑byte trailer. |
| DATA  `0x02` | `u8 op` `u32 offset` `bytes…` | Append a chunk. `offset` **must equal** the running byte count (send in order). Chunk ≤ `ATT_MTU − 3 − 5`. |
| END   `0x03` | `u8 op` `u32 crc32` | CRC‑32/IEEE over the **image bytes only** (= the trailer's `crc32`, i.e. `total_size − 32` bytes). Badge verifies size + trailer + CRC, marks the slot pending, and reboots ~1.2 s later. |

**Behavior & rules**
- Send DATA **in order**; a gap (offset ≠ bytes received) aborts the transfer. Reliable writes
  (Write‑With‑Response) are recommended.
- On END the badge verifies the byte count, the trailer magic/length, and the CRC; if any fails it
  returns an ATT error and stays on the old slot. The inactive slot is written raw — the **active
  slot is never touched**, so a power fail mid‑update simply leaves the running image intact.
- After a successful END the link **drops** when the badge reboots (~1.2 s) — treat a disconnect
  right after END as success, then re‑scan/reconnect.
- The new image **self‑confirms** once it boots far enough to bring BLE/mesh up. An image that
  crashes/resets, **or hangs**, before confirming is **auto‑reverted**: the bootloader counts the
  trial boot and arms the watchdog, so the next boot drops the unconfirmed image and runs the
  previous confirmed slot.
- Do this in config mode (badge awake); each DATA resets the awake window so it won't sleep mid‑update.

Upload sequence (JS):
```js
const OTA   = 'f0de0009-4b1c-4e2a-9a11-a1b2c3d4e5f6';
const OTAST = 'f0de000a-4b1c-4e2a-9a11-a1b2c3d4e5f6';
const st = new DataView((await (await svc.getCharacteristic(OTAST)).readValue()).buffer);
const inactive = st.getUint8(1);                 // 0 -> send slotA.bin, 1 -> slotB.bin
const img = inactive === 0 ? slotA : slotB;      // raw image + 32-byte trailer
const crc = crc32(img.slice(0, img.length - 32)); // CRC over image bytes only (= trailer crc32)

const c = await svc.getCharacteristic(OTA);
const u32 = v => { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, v, true); return b; };
await c.writeValueWithResponse(Uint8Array.of(0x01, ...u32(img.length)));        // START (incl. trailer)
const CH = 240;                                                                 // ≤ MTU-3-5
for (let off = 0; off < img.length; off += CH) {
  const part = img.slice(off, off + CH);
  await c.writeValueWithResponse(Uint8Array.of(0x02, ...u32(off), ...part));    // DATA
}
await c.writeValueWithResponse(Uint8Array.of(0x03, ...u32(crc)));               // END (badge reboots)
```

---

## 11. Mesh control & heartbeat (event‑wide)

Separately from the per‑badge GATT path, an event controller can broadcast over the **mesh** to all
badges at once. Mesh‑reachable commands: **set name**, **set text screen**, **display screen**,
**set attendee/table ID**, **set per‑frame LED**. **Image uploads are GATT‑only.** Mesh
text/display delivery is **best‑effort** (unacked flooding) — use GATT when you need a guaranteed
result. (Vendor opcodes, company ID `0x0059`: name `0x01`, set‑screen `0x08`/`0x09`, display `0x0A`,
attendee `0x0B`, frame‑LED `0x0C` with the same `kind,idx,anim,r,g,b` payload as the GATT char.)

**Heartbeat:** the controller sends an *event heartbeat* mesh message about **once a minute**. A
badge that is **already awake** resets its ~3‑minute window on each heartbeat, so it stays awake to
receive commands for as long as heartbeats continue. A heartbeat **cannot wake a sleeping badge**
(its radio is off in deep sleep) — the attendee still wakes it with the button; heartbeats only
keep an open window open.

---

## 12. Limitations & gotchas (please read)

- **No success/failure response.** The Config Service has no notify characteristic, so the app
  cannot read back whether the CRC passed. Treat a completed write sequence as "sent," and tell the
  user to confirm the badge screen updated. (A status characteristic could be added to the firmware
  later if you need programmatic confirmation.)
- **Config window times out.** The badge stays awake ~3 minutes, refreshed on each
  write/connection event. A long idle gap (e.g. the user wandering off mid‑draw) can let it sleep;
  if a write fails, prompt the user to press the button and reconnect.
- **Name vs image ordering.** Setting the name redraws the text screen and clears the image; upload
  the image **after** the name if you want the image to remain.
- **Always send a full 5808‑byte frame.** Unsent bytes are black; there is no partial/region update.
- **MTU varies by platform** — keep DATA chunks ≤ ~180 bytes for cross‑platform safety; never assume
  a chunk size without handling shorter MTUs.
- **HTTPS + user gesture** are required by Web Bluetooth; iOS needs the **Bluefy** browser.

---

## 13. Quick reference

```
Device name (advertised in config mode): vibamix-<CODE>   e.g. vibamix-1A2F
QR URL:                                  https://<host>/?id=<CODE>

Service     f0de0001-…   (base …-4b1c-4e2a-9a11-a1b2c3d4e5f6)
  Name      f0de0003-…   write UTF-8, <=31 bytes
  Image     f0de0002-…   write / write-no-response  (render-only 1bpp)
  Screen    f0de0004-…   write  (store text screen)
  Image-slot f0de0005-…  write / write-no-response  (store image slot)
  Display   f0de0006-…   write  u8 kind, u8 idx
  Attendee  f0de0007-…   write UTF-8, <=10 bytes  (table/seat ID -> identity screen)
  Frame-LED f0de0008-…   write  u8 kind, u8 idx, u8 anim, u8 r, u8 g, u8 b
  OTA       f0de0009-…   write / write-no-response  (trailered image -> inactive slot, reboot)
  OTA-status f0de000A-…  read   u8 active_slot, u8 inactive_slot, u32 active_version

Image (f0de0002) / Image-slot (f0de0005) frames (little-endian):
  Image      START 0x01 | u16 size(=5808) | u16 w(=176) | u16 h(=264)
  Image-slot START 0x01 | u8 slot(0-3) | u8 format(1=BW 5808 / 2=gray2 11616) | u16 size | u16 w | u16 h
             DATA  0x02 | u16 offset | bytes...
             END   0x03 | u32 crc32(IEEE over all image bytes)

Screen (f0de0004) frames:
  START 0x01 | u8 idx(0-19) | u8 hlen | header bytes
  DATA  0x02 | u16 offset | body bytes
  END   0x03

Display (f0de0006):  u8 kind(0=text screen,1=image slot) | u8 idx
Attendee (f0de0007): UTF-8 string, <=10 bytes
Frame-LED (f0de0008): u8 kind | u8 idx | u8 anim | u8 r | u8 g | u8 b
  anim: 0=off/no-override 1=solid 2=rainbow 3=wheel 4=breathe 5=comet 6=sparkle

OTA (f0de0009) frames (little-endian, u32 size/offset — image ~360 KB):
  START 0x01 | u32 total_size            (raw image + 32-byte CRC trailer)
  DATA  0x02 | u32 offset | bytes...      (offset == bytes sent so far; in order)
  END   0x03 | u32 crc32(IEEE over image bytes only = total_size-32 = trailer crc32)
  Read f0de000A first; send build/slotA.bin or build/slotB.bin for the INACTIVE slot.
  Badge verifies + reboots ~1.2 s after END; auto-reverts if the new image can't confirm.

1bpp framebuffer: 5808 bytes = 264 rows x 22 bytes, MSB=leftmost, bit 1=white / 0=black.
  Pack landscape (dx∈0..263, dy∈0..175): idx = (263-dx)*22 + (dy>>3); mask = 0x80>>(dy&7).
2-bit grayscale: 11616 bytes, 4 px/byte MSB-first, pixel_index=dy*264+dx, level 0=black..3=white.

Mesh (event-wide broadcast): set-name, set-screen, display-screen, set-attendee(0x0B),
  set-frame-led(0x0C) (best-effort); image = GATT only.
  Heartbeat (~60s) keeps an already-awake badge awake; cannot wake a sleeping badge.
```
