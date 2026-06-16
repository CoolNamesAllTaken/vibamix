# Vibamix Badge — BLE Configuration API

This document specifies the Bluetooth Low Energy interface a **configuration app** uses to set up a
vibamix badge: connect to a specific badge, set the attendee **name**, and upload a hand-drawn
**badge image** to the e‑paper screen.

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
| **Name**  | `f0de0003-4b1c-4e2a-9a11-a1b2c3d4e5f6` | Write | UTF‑8 string, **≤ 31 bytes** (longer is truncated). Sets the attendee name and redraws the identity screen. |
| **Image** | `f0de0002-4b1c-4e2a-9a11-a1b2c3d4e5f6` | Write, Write‑Without‑Response | Framed image-upload commands (see §5). |

All multi‑byte integers in payloads are **little‑endian**.

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

## 7. Limitations & gotchas (please read)

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

## 8. Quick reference

```
Device name (advertised in config mode): vibamix-<CODE>   e.g. vibamix-1A2F
QR URL:                                  https://<host>/?id=<CODE>

Service  f0de0001-4b1c-4e2a-9a11-a1b2c3d4e5f6
  Name   f0de0003-4b1c-4e2a-9a11-a1b2c3d4e5f6   write UTF-8, <=31 bytes
  Image  f0de0002-4b1c-4e2a-9a11-a1b2c3d4e5f6   write / write-no-response

Image command frames (little-endian):
  START 0x01 | u16 size(=5808) | u16 width(=176) | u16 height(=264)
  DATA  0x02 | u16 offset      | bytes...
  END   0x03 | u32 crc32(IEEE over all 5808 bytes)

Framebuffer: 5808 bytes = 264 rows x 22 bytes, MSB=leftmost, bit 1=white / 0=black.
Pack landscape (dx∈0..263, dy∈0..175): idx = (263-dx)*22 + (dy>>3); mask = 0x80>>(dy&7).
```
