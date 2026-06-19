# Vibamix Badge — BLE Configuration API

This document specifies the Bluetooth Low Energy interface a **configuration app** uses to set up a
vibamix badge: connect to a specific badge, set the **identity** (name + table ID + LED), upload
hand‑drawn **badge images** (B/W or grayscale) into **4 storage slots**, store up to **20 text
frames**, and command which stored frame the badge displays. It also covers the event‑wide **mesh**
control path (best‑effort broadcast of LED / render‑only image / ephemeral text + the keep‑awake
heartbeat).

It is a complete handoff spec — you should not need the firmware source to implement the app.

---

## 1. How it works (overview)

- The badge spends most of its life powered off. **The attendee presses the button** on the badge;
  it wakes into **config mode**, shows a unique 4‑character **code** and a **QR code**, and becomes
  connectable for ~3 minutes (the window resets on activity).
- The QR encodes a **native‑app deep link**: `vibamix://connect?name=vibamix-<CODE>`
  (e.g. `vibamix://connect?name=vibamix-1A2F`). Scanning it opens the **vibamix configuration app**,
  which connects to the badge by its advertised BLE name **`vibamix-<CODE>`**. The same `<CODE>` is
  shown in plain text on screen so the user can pick the right device if needed.
- The app connects over **GATT** to a custom **Config Service** and writes the badge's content
  (identity name + table ID + LED, text frames, image frames) and can stream a firmware update.
- This is a **direct GATT connection** to one badge. It is independent of the badge mesh network;
  you do not need to know anything about the mesh to configure a single badge.

### Primary consumer: the native app
The intended client is a **native mobile app** (Android/iOS) registered for the `vibamix://` URL
scheme, so scanning the QR launches it straight into the connect flow. A native app has full BLE
access on both platforms and no HTTPS/user‑gesture constraints.

### Optional fallback: Web Bluetooth
The same GATT service can be driven from a **Web Bluetooth** page (`navigator.bluetooth`) where a
native app isn't available. This path is a fallback and has real limits:
- ✅ Chrome / Edge on **Android, Windows, macOS, Linux, ChromeOS**.
- ❌ **iOS/iPadOS Safari does not support Web Bluetooth** — iPhone/iPad users would need the
  **Bluefy** browser app (free).
- Requires the page served over **HTTPS** (or `localhost`); `requestDevice()` must run from a
  **user gesture**. The JS snippets in this doc are reference implementations for that fallback.

---

## 2. Discovery & connection

The deep link carries the target name (`vibamix://connect?name=vibamix-<CODE>`). Parse the `name`
query param, then **scan for and connect to that GAP name**:

1. Start a BLE scan filtering by the **`vibamix-` name prefix** (or the exact `vibamix-<CODE>`).
2. Connect to the matched peripheral, discover the **Config Service** `f0de0001-…`, and use its
   characteristics (§3).

Notes:
- The badge must be **in config mode** (button pressed) to be connectable. If nothing shows up,
  tell the user to press the badge button again.
- **Discover by the `vibamix-` name**, not by the Config Service UUID (see the box below).
- There is no pairing/bonding requirement — writes are unauthenticated.

> ### ⚠️ Discover by name, not by service UUID
>
> In config mode the badge advertises a **legacy connectable + scannable** packet: the Config
> Service UUID `f0de0001-…` (18 bytes) is in the **primary advertising data**, and the
> `vibamix-<CODE>` device name is in the **scan response**. The 128‑bit UUID + name don't both fit
> in one 31‑byte legacy AD, so the name rides in the scan response — meaning a passive scan that
> doesn't request scan responses may see the UUID but not the name, and some hosts (notably
> **macOS CoreBluetooth**) don't surface 128‑bit service UUIDs from every advert.
>
> **For your app:** filter discovery on the **`vibamix-` name prefix** (reliable across platforms),
> and use the Config Service UUID only **after** connecting — it is always present in the badge's
> GATT table and found by normal service discovery on the open connection, independent of what was
> advertised. (There is **no** SIG Mesh GATT Proxy service anymore — the badge is connectable only
> in config mode, never via an always‑on proxy advert.)

### Web Bluetooth fallback (optional)
For the Web Bluetooth path, request the device from a user‑gesture click handler and declare the
service so you may use it after connecting:

```js
const SVC = 'f0de0001-4b1c-4e2a-9a11-a1b2c3d4e5f6';

const device = await navigator.bluetooth.requestDevice({
  filters: [{ namePrefix: 'vibamix-' }],   // or { name: `vibamix-${code}` } from the deep link
  optionalServices: [SVC],
});
const server  = await device.gatt.connect();
const service = await server.getPrimaryService(SVC);
// then getCharacteristic(...) per §3
```

---

## 3. GATT service & characteristics

**Config Service** — UUID `f0de0001-4b1c-4e2a-9a11-a1b2c3d4e5f6`

The service is organized as **one read/write characteristic per *frame type*** (identity, text,
image), plus a quick‑render image, OTA, keepalive, and a mesh‑TX gateway. Each frame characteristic
carries the **whole frame**, supports both **write** (configure) and **read** (load current), and
folds in a "display now" op. Writes are tagged by a leading **op byte** (§3.1); read‑back is
SELECT‑then‑read (§9.4).

| Characteristic | UUID | Properties | Payload |
|----------------|------|-----------|---------|
| **Image** (quick) | `f0de0002-…` | Write, Write‑Without‑Response | Render‑only 1‑bit image upload (see §5). Does **not** persist to a slot. |
| **Identity frame** | `f0de0003-…` | Read, Write, Write‑Without‑Response | Name + table ID + LED (via `META`), an optional gray2 identity image (chunked), `DISPLAY`, and read‑back. See §4. |
| **Text frame** | `f0de0004-…` | Read, Write, Write‑Without‑Response | Store/show/read one of **20 text frames** (header + body + LED). See §7. |
| **Image frame** | `f0de0005-…` | Read, Write, Write‑Without‑Response | Store/show/read one of **4 image slots** (1‑bit B/W or 2‑bit grayscale, + LED). See §8. |
| **OTA** | `f0de0009-…` | Write, Write‑Without‑Response | Firmware update: stream the trailered image for the inactive direct‑XIP slot, then reboot (see §10). |
| **OTA‑status** | `f0de000A-…` | Read | `u8 active_slot, u8 inactive_slot, u32 active_version` — tells the host which slot image to send (see §10). |
| **Keepalive** | `f0de000B-…` | Write, Write‑Without‑Response, **Notify** | 1‑byte liveness counter (see §3.2). |
| **Mesh‑TX** | `f0de000C-…` | Write, Write‑Without‑Response | Event‑wide gateway: re‑originate a vendor‑model access payload onto the mesh group (see §11). |

(All UUIDs share the base `…-4b1c-4e2a-9a11-a1b2c3d4e5f6`.) All multi‑byte integers in payloads are
**little‑endian**.

There is no per‑write success/failure response, but the **Keepalive** characteristic **does notify**
(it's the only notify char) — it's a connection‑liveness ping, not a per‑command ack. Command success
is otherwise visible on the e‑paper screen (and the badge logs to its UART).

### 3.1 Op bytes (first byte of every frame‑characteristic write)

| Op | Value | Meaning |
|----|-------|---------|
| `START`   | `0x01` | begin a chunked payload (image pixels / text body) |
| `DATA`    | `0x02` | a chunk |
| `END`     | `0x03` | commit |
| `META`    | `0x10` | small metadata write (identity: name+id+LED; text/image: LED‑only) |
| `DISPLAY` | `0x20` | show this frame on the panel now |
| `SELECT`  | `0x21` | serialize this frame so the next read returns it |
| `READ_AT` | `0x22` | set the read‑window base (`le16 offset`) for the next read |

The quick **Image** (`f0de0002`) and **OTA** (`f0de0009`) characteristics use the same
`START/DATA/END` op bytes but are not frame characteristics (no META/DISPLAY/SELECT).

### 3.2 Keepalive (`f0de000B`)

A per‑connection liveness ping, distinct from the mesh heartbeat. The app writes a 1‑byte counter
~1 Hz (app→badge liveness) and the badge **notifies** a 1‑byte counter ~1 Hz (badge→app liveness).
Subscribe to its notifications and write it on a 1 s timer; the badge surfaces app‑liveness as a dot
on its connected screen. The value attribute is the **last** characteristic in the service.

---

## 4. The identity frame (`f0de0003`) — name, table ID, LED

The identity frame is the badge's home screen. Set its **name + table ID + LED** in one small write
with the `META` op (`0x10`) to the **Identity frame** characteristic:

```
META  = 0x10, u8 namelen, name…, u8 idlen, id…, u8 anim, u8 r, u8 g, u8 b
```

- `name` — UTF‑8, length‑prefixed; **≤ 31 bytes** stored (extra dropped).
- `id` — UTF‑8 table/seat ID, length‑prefixed; **≤ 10 bytes** stored. Shown as `Table <id>` under
  the name.
- `anim, r, g, b` — the identity frame's LED (anim codes in §9.2; `0` = no override).
- Writing `META` persists all three and **redraws the identity screen**. Both strings persist across
  power cycles.

The identity frame can also carry an **optional full‑screen 2‑bit‑grayscale identity image**, sent
with the chunked `START/DATA/END` ops on this same characteristic (the banner is composited over it):

```
START = 0x01, u8 fmt(=2 gray2), u16 size, u16 w, u16 h     (then DATA / END as in §5/§8)
```

To show the identity frame on the panel immediately, write `DISPLAY` (`0x20`). To read the current
identity back, use `SELECT`/`READ_AT` (§9.4).

> Note: a quick **render‑only** image (not the identity slot, not persisted) goes to the separate
> **Image** characteristic `f0de0002` — see §5.

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

## 7. Text frames (stored content)

The badge stores **20 text frames**, indexed `0…19`, each a **header** (≤ 47 bytes), a **body**
(≤ 1023 bytes, word‑wrapped on the badge), and a per‑frame **LED**. They persist across power cycles.
Set one with a framed stream to the **Text frame** characteristic (`f0de0004`):

| Op | Payload | Meaning |
|----|---------|---------|
| `0x01` START | `u8 idx`, `u8 anim`, `u8 r`, `u8 g`, `u8 b`, `u8 hlen`, `hlen` header bytes | Begin frame `idx`; carries the LED + the full header. |
| `0x02` DATA  | `u16 offset`, body bytes | Body bytes at `offset` (chunk for bodies > one MTU). |
| `0x03` END   | — | Commit (store LED + header + accumulated body). |
| `0x10` META  | `u8 idx`, `u8 anim`, `u8 r`, `u8 g`, `u8 b` | Update **only** this frame's LED (no body resend). |
| `0x20` DISPLAY | `u8 idx` | Show text frame `idx` on the panel now. |
| `0x21` SELECT | `u8 idx` | Serialize frame `idx` for read‑back (§9.4). |

The body is reassembled by offset (like an image), so cover `0…blen‑1`; short bodies can be a single
DATA at offset 0. `START/…/END` only **stores** the frame — use `DISPLAY` (or the §9 Display flow) to
show it.

---

## 8. Image slots (4 stored images, B/W or grayscale)

The badge stores **4 full‑screen image slots**, indexed `0…3`. Each slot is either:
- **1‑bit B/W** (`format = 1`) — the panel‑native 5808‑byte framebuffer (§6 packing). Displayed by a
  direct blit (crisp; pre‑dither on the host for best art).
- **2‑bit grayscale** (`format = 2`) — a 11,616‑byte image the badge **dithers to B/W** on display
  (the panel is physically 1‑bit, so grayscale is approximated with an ordered dither).

Upload to the **Image frame** characteristic (`f0de0005`), same framing as §5 but START carries the
slot, format, and the per‑frame LED:

| Op | Payload | Meaning |
|----|---------|---------|
| `0x01` START | `u8 slot`, `u8 format`, `u8 anim`, `u8 r`, `u8 g`, `u8 b`, `u16 size`, `u16 width`, `u16 height` | Begin upload to `slot` (0–3) + set its LED. B/W: `size=5808`, `width=176`, `height=264`. Gray: `size=11616`, **`width=264`, `height=176`** (the landscape packing stride — the dither reads `pixel_index = dy*width + dx`). |
| `0x02` DATA  | `u16 offset`, image bytes | As §5. |
| `0x03` END   | `u32 crc32` | CRC‑32/IEEE over the whole image; on success the slot is stored **and** displayed. |
| `0x10` META  | `u8 slot`, `u8 anim`, `u8 r`, `u8 g`, `u8 b` | Update **only** this slot's LED (no image resend). |
| `0x20` DISPLAY | `u8 slot` | Show image slot `slot` on the panel now. |
| `0x21` SELECT | `u8 slot`, `u8 want_pixels` | Serialize the slot for read‑back (§9.4). |

**2‑bit packing.** Author on the same 264 × 176 landscape canvas as §6. Pack **4 pixels per byte,
MSB‑first, row‑major**: `pixel_index = dy*264 + dx`; the 2‑bit level lives in bits
`[7:6],[5:4],[3:2],[1:0]` of byte `pixel_index >> 2`. Level **0 = black … 3 = white**. Total
= 264·176·2/8 = **11,616 bytes**.

> Images are **local‑GATT only** — they are never sent over the mesh (too large to flood).

---

## 9. Displaying a stored frame

There is **no** separate Display characteristic — show a stored frame with the `DISPLAY` op
(`0x20`) on the frame's own characteristic:
- **Identity:** `DISPLAY` (no index) on `f0de0003`.
- **Text frame:** `DISPLAY, u8 idx` (0–19) on `f0de0004`.
- **Image slot:** `DISPLAY, u8 slot` (0–3) on `f0de0005`.

The rendered frame takes over the panel (the config countdown stops repainting). Displaying an empty
slot/frame is a no‑op.

### 9.1 Attendee / table ID

The table/seat ID is **part of the identity frame**, set together with the name via the identity
`META` write (§4): `META = 0x10, namelen, name…, idlen, id…, anim, r, g, b`. It is stored **≤ 10
bytes**, persists, and shows on the identity screen as `Table <id>` below the name. (There is no
standalone attendee characteristic.)

### 9.2 Per‑frame LED animation + color

Each frame carries its own LED animation + color, shown on the 4 badge LEDs **while that frame is the
displayed one**. The LED is set **with the frame**, not via a separate characteristic:
- **Identity:** the `anim,r,g,b` in its `META` write (§4).
- **Text / image frame:** the `anim,r,g,b` in `START` (§7/§8), or updated alone with `META`
  (`0x10, idx/slot, anim, r, g, b`) without resending the body/image.

`anim` codes: `0` Off/*no override*, `1` Solid, `2` Rainbow, `3` Wheel, `4` Breathe, `5` Comet,
`6` Sparkle. Solid/Breathe/Comet/Sparkle use `r,g,b`; Rainbow/Wheel ignore it (firmware caps
brightness). `anim=0` means "no override" (fall back to the badge default), not a forced blackout.
The LED applies when that frame is displayed (live if it's the current frame); the strip runs during
the badge's awake window.

### 9.3 Connected indicator

While a phone/laptop is connected over this service the badge shows a **"Connected"** (or
**"Mesh Gateway"** once it has relayed a mesh command) screen and **does not time out** — it stays in
config until the link drops. **On disconnect the badge leaves config mode entirely** and returns to
its **home identity frame** with the heartbeat‑driven mesh‑mode countdown bar; with no event
heartbeats that bar runs out (~60 s) and the badge sleeps. (Re‑entering config needs another button
press.)

### 9.4 Reading a frame back (SELECT → READ_AT → read)

The identity (`f0de0003`), text (`f0de0004`) and image‑slot (`f0de0005`) characteristics are also
**readable** — to load what's currently stored (e.g. to re‑edit it, or preview a stored image). The
flow is:

1. **SELECT** — write `0x21` plus a frame selector to serialize that frame into the badge's read
   buffer:
   - identity: `0x21, u8 want_pixels`
   - text: `0x21, u8 idx`
   - image slot: `0x21, u8 slot, u8 want_pixels`
2. **Read in windows** — repeat: write `READ_AT` `0x22, u16 offset` to set the read base, then **read**
   the characteristic (returns the bytes from `offset` onward, up to one MTU). Append each chunk and
   advance `offset` by its length; **stop when a read returns 0 bytes** (offset past the end). This
   windowing is **mandatory**: a single GATT characteristic value is capped at **512 bytes** and
   macOS/CoreBluetooth enforces that, but an image frame is multi‑KB.

Serialized layouts (all little‑endian; `present`/`len` describe the stored frame):

- **identity:** `u8 namelen, name…, u8 idlen, id…, u8 anim,r,g,b, u8 present, u8 fmt, u16 w, u16 h,
  u16 len, [pixels if want_pixels]`.
- **text:** `u8 present, u8 idx, u8 anim,r,g,b, u8 hlen, header…, u16 blen, body…`.
- **image slot:** `u8 present, u8 slot, u8 anim,r,g,b,` then the image block
  `u8 present, u8 fmt, u16 w, u16 h, u16 len, [pixels if want_pixels]`. **Note the duplicate
  `present` byte** — the leading `present`/`slot` precede the same image block that the identity read
  ends with.

`SELECT` resets the read window to 0; you can re‑read a frame by issuing `READ_AT 0` again.

---

## 10. Firmware update (OTA)

> This section is the **BLE wire framing** for streaming an update. The bootloader internals, the
> app **image/trailer layout**, `bl_state`, and the **`.ota` file structure** are documented in
> [bootloader-and-image-format.md](bootloader-and-image-format.md).

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
  trial boot and arms a watchdog (**30 s** to reach confirm), giving the image up to
  **3 trial boots** before it drops the unconfirmed image and runs the previous confirmed slot.
- Do this in config mode (badge awake); each DATA resets the awake window so it won't sleep mid‑update.

### Recovery, rollback & boot diagnostics

Two gotchas can make an OTA *look* like it bricked the badge — both are about the **other** slot,
not the new image:

- **Rollback needs a trailered image in the other slot.** Auto‑revert can only fall back to a slot
  whose CRC trailer verifies. An app `load`ed by a debugger (VS Code **F5** / `gdb load`) has **no
  trailer**, so it is *not* a valid revert target. To have a real fallback, SWD‑flash a trailered
  `slotA.bin` instead — e.g. `probe-rs download --binary-format bin --base-address 0xE000 build/slotA.bin`
  — not F5. (If no slot is a valid revert target, the bootloader now **trial‑boots the only
  CRC‑valid image it can find** rather than halting — a slow retry beats a dead board.)
- **Detach the debugger before triggering OTA.** While a probe is attached the bootloader takes its
  dev path and boots **slot A** (the old image) regardless of `bl_state`, so a freshly‑OTA'd slot B
  silently won't run; and `sys_reboot()` under a halted/attached probe may not actually reset. Close
  the debug session first, then OTA.

**On‑screen boot diagnostics.** The board has no usable serial, so on a **trial boot**, a **revert**,
or a **halt** the bootloader draws a diagnostic screen on the ePaper before chain‑loading: the boot
decision (`TRIAL SLOT B` / `REVERT TO A` / `HALT NO BOOTABLE IMG`) and, per slot A/B, `VALID`,
live `CRC OK/FAIL`, `VER`, `LEN`, `ATT` (trial count) and `CONF`/`UNCONF`. A normal confirmed boot
is **not** slowed by this (no extra refresh). If a badge sticks after an update, read this screen to
see which slot was chosen and why.

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
badges at once. The mesh surface is deliberately small — **ephemeral, broadcast‑only** commands;
all **stored** content (name, table ID, stored text/image frames, per‑frame LED, display selection)
is **GATT‑only** now. Mesh delivery is **best‑effort** (unacked flooding) — use GATT when you need a
guaranteed result.

Vendor opcodes (company ID `0x0059`, 3‑byte `0x_C0_<b0>_<cid>`):

| Opcode | Name | Payload | Effect |
|-------:|------|---------|--------|
| `0x03` | SET_LED | `anim, r, g, b` | Set the LED live on every badge (not stored). |
| `0x04` / `0x05` / `0x06` | IMG START / DATA / END | `le16 size,w,h` / `le16 off,bytes` / `le32 crc32` | Flood a **render‑only** 1‑bit image (best‑effort; a dropped segment fails the frame). |
| `0x07` | HEARTBEAT | optional UTF‑8 event name (≤ 8 B) | Keep‑awake beat (below). |
| `0x08` / `0x09` | SHOW_TEXT hdr / body | header / `seq,last,body` | Draw an **ephemeral** text frame (not stored). |

> Removed opcodes (now GATT‑only): `0x01` set‑name, `0x02` set‑fun‑fact, `0x0A` display‑stored,
> `0x0B` set‑attendee, `0x0C` set‑frame‑LED.

**Laptop / phone as a mesh gateway.** A host with no mesh radio injects these by writing a complete
vendor‑model access payload (the 3‑byte opcode + params) to the **Mesh‑TX** characteristic
(`f0de000C`) of a **config‑mode** badge; that badge re‑originates it onto the `0xC000` "all badges"
group. (This replaces the old SIG Mesh GATT Proxy ingress, which no longer exists.)

**Heartbeat (opcode `0x07`):** the controller floods an *event heartbeat* across the mesh at **1 Hz**.
Its payload is a short **UTF‑8 event name** (**≤ 8 bytes** — longer would split the mesh access
payload into multiple segments, multiplying the 1 Hz flood; keep it short to stay a single
unsegmented flood). An empty payload is allowed (nameless beat).

Two effects on a badge that is **already awake**:

- **Config mode** (button‑woken, GATT path): each beat resets the ~3‑minute config window, as before.
- **Mesh mode** (the normal awake path, *not* GATT‑connected for config): the badge stays awake for as
  long as beats keep arriving and shows a **thin countdown bar along the bottom** of the screen with
  the latched event name and the seconds until power‑off. Each beat resets that timeout to **60 s**;
  when beats stop, the bar runs out and the badge redraws a clean frame and sleeps.

A heartbeat **cannot wake a sleeping badge** (its radio is off in deep sleep) — the attendee wakes it
with the button (or it is already awake from a recent boot); heartbeats only keep an awake badge
awake. Because the device latches the last name and the timeout tolerates ~60 missed beats, an
occasional dropped flood is harmless.

---

## 12. Limitations & gotchas (please read)

- **No per‑command ack.** The only notify characteristic is **Keepalive** (`f0de000B`, a
  connection‑liveness ping) — there is no per‑write status, so the app cannot read back whether a
  CRC passed. Treat a completed write sequence as "sent," and tell the user to confirm the badge
  screen updated. (To read back what's *stored*, use the SELECT→READ_AT flow in §9.4.)
- **Config window times out.** The badge stays awake ~3 minutes, refreshed on each
  write/connection event. A long idle gap (e.g. the user wandering off mid‑draw) can let it sleep;
  if a write fails, prompt the user to press the button and reconnect.
- **Name vs quick image ordering.** Writing the identity `META` (name/ID/LED) redraws the identity
  screen, which replaces a quick render‑only image (`f0de0002`); send the quick image **after** the
  identity write if you want it to remain. (A persisted image *slot* shown via `DISPLAY` is the
  durable way to keep an image on screen.)
- **Always send a full 5808‑byte frame.** Unsent bytes are black; there is no partial/region update.
- **MTU varies by platform** — keep DATA chunks ≤ ~180 bytes for cross‑platform safety; never assume
  a chunk size without handling shorter MTUs.
- **Don't filter discovery on the Config Service UUID** — filter by the `vibamix-` name and use the
  service only after connecting (see the box in §2).
- **Web Bluetooth fallback only:** that path needs **HTTPS + a user gesture**, and iOS needs the
  **Bluefy** browser. The native app has neither constraint.

---

## 13. Quick reference

```
Device name (advertised in config mode): vibamix-<CODE>   e.g. vibamix-1A2F
QR deep link:                            vibamix://connect?name=vibamix-<CODE>

Service       f0de0001-…   (base …-4b1c-4e2a-9a11-a1b2c3d4e5f6)
  Image       f0de0002-…   write / write-no-resp   (quick render-only 1bpp)
  Identity    f0de0003-…   read/write              (name+ID+LED via META; optional gray2 image)
  Text frame  f0de0004-…   read/write              (store/show/read text frame 0-19)
  Image frame f0de0005-…   read/write / write-no-resp  (store/show/read image slot 0-3)
  OTA         f0de0009-…   write / write-no-resp   (trailered image -> inactive slot, reboot)
  OTA-status  f0de000A-…   read   u8 active, u8 inactive, u32 active_version
  Keepalive   f0de000B-…   write / write-no-resp / NOTIFY   (1-byte liveness counter; last char)
  Mesh-TX     f0de000C-…   write / write-no-resp   (re-originate a vendor access payload to mesh)

Op bytes (frame chars 0003/0004/0005):
  0x01 START | 0x02 DATA | 0x03 END | 0x10 META | 0x20 DISPLAY | 0x21 SELECT | 0x22 READ_AT

Identity (f0de0003):
  META 0x10 | u8 namelen,name | u8 idlen,id | u8 anim,r,g,b
  image START 0x01 | u8 fmt(=2) | u16 size | u16 w | u16 h   (then DATA/END)
  DISPLAY 0x20 | SELECT 0x21 u8 want_pixels | READ_AT 0x22 u16 off

Quick Image (f0de0002) frame (little-endian):
  START 0x01 | u16 size(=5808) | u16 w(=176) | u16 h(=264)
  DATA  0x02 | u16 offset | bytes...
  END   0x03 | u32 crc32(IEEE over all image bytes)

Text frame (f0de0004):
  START 0x01 | u8 idx(0-19) | u8 anim,r,g,b | u8 hlen | header
  DATA  0x02 | u16 offset | body | END 0x03
  META 0x10 | u8 idx | u8 anim,r,g,b      (LED only)
  DISPLAY 0x20 u8 idx | SELECT 0x21 u8 idx

Image frame (f0de0005):
  START 0x01 | u8 slot(0-3) | u8 fmt(1=BW 5808 / 2=gray2 11616) | u8 anim,r,g,b | u16 size | u16 w | u16 h
  DATA  0x02 | u16 offset | bytes... | END 0x03 | u32 crc32
  META 0x10 | u8 slot | u8 anim,r,g,b      (LED only)
  DISPLAY 0x20 u8 slot | SELECT 0x21 u8 slot, u8 want_pixels
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

Mesh (event-wide broadcast, best-effort): set-LED(0x03 anim,r,g,b), render-only image(0x04-0x06),
  show-text(0x08/0x09, ephemeral), heartbeat(0x07). Stored content + display = GATT only.
  A laptop injects mesh via the Mesh-TX char (f0de000C) on a config-mode badge.
  Heartbeat (~60s) keeps an already-awake badge awake; cannot wake a sleeping badge.
```
