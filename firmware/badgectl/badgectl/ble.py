"""BLE transport: scan for badges and drive one connection (GATT + proxy)."""

from __future__ import annotations

import asyncio
import struct
import zlib
from dataclasses import dataclass

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakDeviceNotFoundError

from . import keys


@dataclass
class Found:
    name: str
    address: str
    is_proxy: bool
    device: object = None  # the BLEDevice from the scan (not comparable)


def _match(dev, adv) -> Found | None:
    name = adv.local_name or dev.name or ""
    svcs = [u.lower() for u in (adv.service_uuids or [])]
    is_proxy = keys.UUID_PROXY_SVC.lower() in svcs
    is_cfg = keys.UUID_CFG_SVC.lower() in svcs
    if name.startswith(keys.DEVICE_NAME_PREFIX) or is_proxy or is_cfg:
        return Found(name=name or "(unknown)", address=dev.address, is_proxy=is_proxy, device=dev)
    return None


async def scan(timeout: float = 8.0) -> list[Found]:
    """Continuously scan, returning as soon as a named `vibamix-` badge appears
    (otherwise the full window). Matches by the config/proxy service UUID — which
    the badge's fast config-mode advert carries in its primary AD — or by name."""
    out: dict[str, Found] = {}
    found_named = asyncio.Event()

    def cb(dev, adv):
        f = _match(dev, adv)
        if f is None:
            return
        out[f.address] = f
        if f.name.startswith(keys.DEVICE_NAME_PREFIX):
            found_named.set()

    async with BleakScanner(detection_callback=cb):
        try:
            await asyncio.wait_for(found_named.wait(), timeout)
        except asyncio.TimeoutError:
            pass
    return sorted(out.values(), key=lambda f: f.name)


class Scanner:
    """Continuous background scan. Calls `on_update(addr, name, rssi, is_proxy,
    device)` for every advertisement from a vibamix badge until stopped — feeds a
    live device table (scales to hundreds)."""

    def __init__(self, on_update) -> None:
        self._on_update = on_update
        self._scanner: BleakScanner | None = None

    @property
    def running(self) -> bool:
        return self._scanner is not None

    async def start(self) -> None:
        if self._scanner is not None:
            return

        def cb(dev, adv):
            f = _match(dev, adv)
            if f is not None:
                self._on_update(f.address, f.name, adv.rssi, f.is_proxy, dev)

        self._scanner = BleakScanner(detection_callback=cb)
        await self._scanner.start()

    async def stop(self) -> None:
        if self._scanner is not None:
            try:
                await self._scanner.stop()
            finally:
                self._scanner = None


def _le16(v: int) -> bytes:
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


def _get_str(v: bytes, o: int) -> tuple[str, int]:
    """Read a u8-length-prefixed UTF-8 string at offset o; return (str, next_offset)."""
    n = v[o]
    return v[o + 1 : o + 1 + n].decode("utf-8", "replace"), o + 1 + n


OTA_MAGIC = b"VOTA"


def parse_ota(data: bytes) -> tuple[int, dict[int, bytes]]:
    """Parse a `.ota` bundle (scripts/pack_ota.py). Returns (app_version, {slot: image}).

    Layout: magic "VOTA", u16 format, u16 slot_count, u32 version, u32 reserved,
    then slot_count x {u8 slot, u8[3] pad, u32 offset, u32 length}, then payloads.
    """
    if len(data) < 16 or data[:4] != OTA_MAGIC:
        raise ValueError("not a .ota bundle (bad magic)")
    fmt, slot_count, version, _rsvd = struct.unpack_from("<HHII", data, 4)
    if fmt != 1:
        raise ValueError(f"unsupported .ota format version {fmt}")
    images: dict[int, bytes] = {}
    for i in range(slot_count):
        slot, off, length = struct.unpack_from("<BxxxII", data, 16 + 12 * i)
        if off + length > len(data):
            raise ValueError(f".ota slot {slot} entry exceeds file size")
        images[slot] = data[off : off + length]
    return version, images


class BadgeLink:
    """One BLE connection; exposes the custom config service + the mesh proxy."""

    def __init__(self) -> None:
        self.client: BleakClient | None = None

    @property
    def connected(self) -> bool:
        return self.client is not None and self.client.is_connected

    @property
    def mtu(self) -> int:
        try:
            return self.client.mtu_size  # type: ignore[union-attr]
        except Exception:
            return 23

    async def connect(self, target, on_disconnect=None) -> None:
        # On macOS you must connect by a live BLEDevice, not a bare address — a
        # stale/address-only handle raises BleakDeviceNotFoundError. Re-resolve a
        # fresh handle right before connecting; fall back to the scanned one.
        if isinstance(target, Found):
            address, device = target.address, target.device
        else:
            address, device = str(target), getattr(target, "device", None)
        fresh = await BleakScanner.find_device_by_address(address, timeout=8.0)
        dev = fresh or device
        if dev is None:
            raise BleakDeviceNotFoundError(address, f"Device {address} is not advertising")
        self.client = BleakClient(dev, timeout=15.0, disconnected_callback=on_disconnect)
        await self.client.connect()

    async def disconnect(self) -> None:
        if self.client is not None:
            try:
                await self.client.disconnect()
            finally:
                self.client = None

    def _chunk(self, header_len: int) -> int:
        return max(20, self.mtu - 3) - header_len

    async def _w(self, uuid: str, data: bytes, response: bool = True) -> None:
        assert self.client is not None
        await self.client.write_gatt_char(uuid, data, response=response)

    async def _read_serialized(self, uuid: str, on_progress=None, total_fn=None) -> bytes:
        """Read a SELECT-serialized frame in <=MTU windows.

        macOS/CoreBluetooth caps a single characteristic read at 512 B, but a
        serialized image frame is multi-KB. The badge exposes an app-level read
        window (GOP_READ_AT): we move the base, read a chunk, and repeat until a
        read past the end returns nothing. Call after the SELECT write.

        Progress: the total length isn't known up front, so `total_fn(buf)` derives
        the expected full size once enough header bytes have arrived (else None);
        `on_progress(done, total)` then mirrors the upload convention."""
        assert self.client is not None
        out = bytearray()
        off = 0
        total = None
        while True:
            await self._w(uuid, bytes([keys.GOP_READ_AT]) + _le16(off))
            chunk = await self.client.read_gatt_char(uuid)
            if not chunk:
                break
            out += chunk
            off += len(chunk)
            if total is None and total_fn is not None:
                total = total_fn(out)
            if on_progress and total:
                on_progress(min(len(out), total), total)
        if on_progress and total:
            on_progress(total, total)
        return bytes(out)

    # --- direct GATT config: one read/write characteristic per frame type ---
    async def _upload(self, uuid, start, payload, crc, on_progress) -> None:
        await self._w(uuid, start)
        chunk = self._chunk(3)  # DATA header = op + u16 offset
        off, n = 0, len(payload)
        while off < n:
            part = payload[off : off + chunk]
            await self._w(uuid, bytes([keys.GOP_DATA]) + _le16(off) + part)
            off += len(part)
            if on_progress:
                on_progress(off, n)
        await self._w(uuid, bytes([keys.GOP_END]) + struct.pack("<I", crc))

    async def upload_image(self, fb: bytes, on_progress=None) -> None:
        """Quick render (f0de0002): draw a 1bpp image now, not stored."""
        crc = zlib.crc32(fb) & 0xFFFFFFFF
        start = bytes([keys.GOP_START]) + _le16(len(fb)) + _le16(176) + _le16(264)
        await self._upload(keys.UUID_CHR_IMAGE, start, fb, crc, on_progress)

    # -- identity frame (f0de0003) --
    async def write_identity_meta(self, name: str, id_str: str,
                                  anim: int, r: int, g: int, b: int) -> None:
        n = name.encode("utf-8")[:31]
        i = id_str.encode("utf-8")[:10]
        payload = (bytes([keys.GOP_META, len(n)]) + n + bytes([len(i)]) + i
                   + bytes([anim, r, g, b]))
        await self._w(keys.UUID_IDENTITY, payload)

    async def upload_identity_image(self, payload: bytes, on_progress=None) -> None:
        """Identity-frame image (gray2), drawn behind the name/ID banner."""
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        start = (bytes([keys.GOP_START, keys.FMT_GRAY2])
                 + _le16(len(payload)) + _le16(264) + _le16(176))
        await self._upload(keys.UUID_IDENTITY, start, payload, crc, on_progress)

    async def read_identity(self, want_pixels: bool = False, on_progress=None) -> dict:
        assert self.client is not None
        await self._w(keys.UUID_IDENTITY, bytes([keys.GOP_SELECT, 1 if want_pixels else 0]))

        def _total(b: bytes):
            try:
                o = 0
                o += 1 + b[o]          # namelen + name
                o += 1 + b[o]          # idlen + id
                o += 4                 # led (anim,r,g,b)
                o += 1 + 1 + 2 + 2     # present, fmt, w, h
                if o + 2 > len(b):
                    return None
                ln = int.from_bytes(b[o:o + 2], "little")
                o += 2
                return o + ln
            except IndexError:
                return None

        v = await self._read_serialized(keys.UUID_IDENTITY,
                                        on_progress=on_progress, total_fn=_total)
        o = 0
        name, o = _get_str(v, o)
        attendee, o = _get_str(v, o)
        led = {"anim": v[o], "r": v[o + 1], "g": v[o + 2], "b": v[o + 3]}
        o += 4
        img = {"present": bool(v[o]), "fmt": v[o + 1],
               "w": int.from_bytes(v[o + 2:o + 4], "little"),
               "h": int.from_bytes(v[o + 4:o + 6], "little")}
        ln = int.from_bytes(v[o + 6:o + 8], "little")
        o += 8
        if want_pixels and img["present"]:
            img["pixels"] = bytes(v[o:o + ln])
        return {"name": name, "attendee": attendee, "led": led, "image": img}

    # -- text frame (f0de0004) --
    async def write_text_frame(self, idx: int, anim: int, r: int, g: int, b: int,
                               title: str, body: str, on_progress=None) -> None:
        h = title.encode("utf-8")[:47]
        bb = body.encode("utf-8")
        await self._w(keys.UUID_TEXT_FRAME,
                      bytes([keys.GOP_START, idx, anim, r, g, b, len(h)]) + h)
        chunk = self._chunk(3)
        off = 0
        while off < len(bb):
            part = bb[off:off + chunk]
            await self._w(keys.UUID_TEXT_FRAME, bytes([keys.GOP_DATA]) + _le16(off) + part)
            off += len(part)
            if on_progress:
                on_progress(off, len(bb))
        await self._w(keys.UUID_TEXT_FRAME, bytes([keys.GOP_END]))

    async def write_text_led(self, idx: int, anim: int, r: int, g: int, b: int) -> None:
        """Update only a text frame's LED (no body resend)."""
        await self._w(keys.UUID_TEXT_FRAME, bytes([keys.GOP_META, idx, anim, r, g, b]))

    async def read_text_frame(self, idx: int) -> dict:
        assert self.client is not None
        await self._w(keys.UUID_TEXT_FRAME, bytes([keys.GOP_SELECT, idx]))
        v = await self._read_serialized(keys.UUID_TEXT_FRAME)
        present = bool(v[0])
        led = {"anim": v[2], "r": v[3], "g": v[4], "b": v[5]}
        o = 6
        hlen = v[o]
        o += 1
        header = v[o:o + hlen].decode("utf-8", "replace")
        o += hlen
        blen = int.from_bytes(v[o:o + 2], "little")
        o += 2
        body = v[o:o + blen].decode("utf-8", "replace")
        return {"present": present, "idx": v[1], "led": led, "header": header, "body": body}

    # -- image frame (f0de0005) --
    async def write_image_frame(self, slot: int, fmt: int, anim: int, r: int, g: int, b: int,
                                payload: bytes, on_progress=None) -> None:
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        w, h = (176, 264) if fmt == keys.FMT_BW else (264, 176)
        start = (bytes([keys.GOP_START, slot, fmt, anim, r, g, b])
                 + _le16(len(payload)) + _le16(w) + _le16(h))
        await self._upload(keys.UUID_IMAGE_FRAME, start, payload, crc, on_progress)

    async def write_image_led(self, slot: int, anim: int, r: int, g: int, b: int) -> None:
        """Update only an image frame's LED (no image resend)."""
        await self._w(keys.UUID_IMAGE_FRAME, bytes([keys.GOP_META, slot, anim, r, g, b]))

    async def read_image_frame(self, slot: int, want_pixels: bool = False,
                               on_progress=None) -> dict:
        assert self.client is not None
        await self._w(keys.UUID_IMAGE_FRAME, bytes([keys.GOP_SELECT, slot, 1 if want_pixels else 0]))
        # 14-byte header (present,slot,led(4),present,fmt,w(2),h(2),len(2)) then pixels.
        total_fn = (lambda b: (14 + int.from_bytes(b[12:14], "little"))
                    if len(b) >= 14 else None)
        v = await self._read_serialized(keys.UUID_IMAGE_FRAME,
                                        on_progress=on_progress, total_fn=total_fn)
        # Layout: present, slot, anim,r,g,b, then put_image() block which leads with
        # its own (duplicate) present byte at v[6] before fmt — hence the +1 offsets.
        present = bool(v[0])
        led = {"anim": v[2], "r": v[3], "g": v[4], "b": v[5]}
        fmt = v[7]
        w = int.from_bytes(v[8:10], "little")
        h = int.from_bytes(v[10:12], "little")
        ln = int.from_bytes(v[12:14], "little")
        out = {"present": present, "slot": v[1], "led": led, "fmt": fmt, "w": w, "h": h}
        if want_pixels and present:
            out["pixels"] = bytes(v[14:14 + ln])
        return out

    # -- force-display a stored frame (folded into each frame characteristic) --
    async def display(self, kind: int, idx: int = 0) -> None:
        if kind == keys.DISP_KIND_IDENTITY:
            await self._w(keys.UUID_IDENTITY, bytes([keys.GOP_DISPLAY]))
        elif kind == keys.DISP_KIND_TEXT:
            await self._w(keys.UUID_TEXT_FRAME, bytes([keys.GOP_DISPLAY, idx]))
        else:
            await self._w(keys.UUID_IMAGE_FRAME, bytes([keys.GOP_DISPLAY, idx]))

    # --- firmware OTA (trailered direct-XIP image -> inactive slot) ---
    async def read_ota_status(self) -> tuple[int, int, int]:
        """Return (active_slot, inactive_slot, active_version) from f0de000A."""
        assert self.client is not None
        v = await self.client.read_gatt_char(keys.UUID_CHR_OTA_STATUS)
        if len(v) < 6:
            raise RuntimeError(f"OTA status too short ({len(v)} bytes)")
        active, inactive = v[0], v[1]
        version = struct.unpack_from("<I", v, 2)[0]
        return active, inactive, version

    async def ota_update_auto(self, build_dir: str, on_progress=None) -> int:
        """Pick slotA.bin/slotB.bin for the badge's inactive slot and flash it.

        Returns the slot index that was updated. The image must have been built
        for that slot (slots are linked at different offsets — direct-XIP)."""
        import os

        _active, inactive, _ver = await self.read_ota_status()
        name = "slotA.bin" if inactive == keys.OTA_SLOT_A else "slotB.bin"
        path = os.path.join(build_dir, name)
        if not os.path.exists(path):
            raise FileNotFoundError(
                f"inactive slot {inactive} needs {name}, not found in {build_dir}")
        await self.ota_update(path, on_progress)
        return inactive

    async def ota_update_file(self, ota_path: str, on_progress=None) -> tuple[int, int]:
        """Flash from a single `.ota` bundle: read which slot is inactive on the
        badge, extract that slot's image from the bundle, and stream it. Returns
        (slot_flashed, app_version)."""
        with open(ota_path, "rb") as f:
            version, images = parse_ota(f.read())
        _active, inactive, _ver = await self.read_ota_status()
        img = images.get(inactive)
        if img is None:
            raise RuntimeError(f".ota bundle has no image for the badge's inactive slot {inactive}")
        await self._ota_stream(img, on_progress)
        return inactive, version

    async def ota_update(self, path: str, on_progress=None) -> None:
        """Stream a single trailered slot image file (raw image + 32-byte CRC trailer)."""
        with open(path, "rb") as f:
            await self._ota_stream(f.read(), on_progress)

    async def _ota_stream(self, data: bytes, on_progress=None) -> None:
        if len(data) <= 32:
            raise RuntimeError("image too small (missing CRC trailer?)")
        # END crc = the trailer's crc32, computed over the image bytes only (the
        # firmware re-derives image_len = total - 32 and verifies against it).
        crc = zlib.crc32(data[:-32]) & 0xFFFFFFFF
        # OTA uses u32 size/offset (image ~360 KB exceeds the u16 used elsewhere).
        await self._w(keys.UUID_CHR_OTA, bytes([keys.FRAME_START]) + struct.pack("<I", len(data)))
        chunk = self._chunk(5)  # DATA header = op + u32 offset
        off, n = 0, len(data)
        while off < n:
            part = data[off : off + chunk]
            await self._w(keys.UUID_CHR_OTA, bytes([keys.FRAME_DATA]) + struct.pack("<I", off) + part)
            off += len(part)
            if on_progress:
                on_progress(off, n)
        # END: the badge verifies the image, marks the slot pending, and reboots
        # ~1.2 s later, so the link drops shortly after — a write/disconnect error
        # here is expected/success.
        try:
            await self._w(keys.UUID_CHR_OTA, bytes([keys.FRAME_END]) + struct.pack("<I", crc))
        except Exception:
            pass

    # --- per-connection keepalive (1 Hz, bidirectional) ---
    async def subscribe_keepalive(self, on_rx) -> None:
        """Subscribe to badge->app keepalive notifications; on_rx(code:int)."""
        def cb(_char, data: bytearray):
            on_rx(data[0] if data else 0)
        await self.client.start_notify(keys.UUID_CHR_KEEPALIVE, cb)

    async def send_keepalive(self, code: int = 0) -> None:
        """Write an app->badge keepalive (write-without-response for speed)."""
        await self._w(keys.UUID_CHR_KEEPALIVE, bytes([code & 0xFF]), response=False)

    # --- mesh injection (config-mode gateway) ---
    async def mesh_tx(self, access: bytes) -> None:
        """Broadcast a vendor-model access payload (opcode + params) to the fleet.
        The connected config-mode badge re-originates it onto the mesh group."""
        await self._w(keys.UUID_MESH_TX, access, response=True)
