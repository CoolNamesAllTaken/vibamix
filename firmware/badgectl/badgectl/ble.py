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


def _le16(v: int) -> bytes:
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


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

    async def connect(self, target) -> None:
        # On macOS you must connect by a live BLEDevice, not a bare address — a
        # stale/address-only handle raises BleakDeviceNotFoundError. Re-resolve a
        # fresh handle right before connecting; fall back to the scanned one.
        if isinstance(target, Found):
            address, device = target.address, target.device
        else:
            address, device = str(target), None
        fresh = await BleakScanner.find_device_by_address(address, timeout=8.0)
        dev = fresh or device
        if dev is None:
            raise BleakDeviceNotFoundError(address, f"Device {address} is not advertising")
        self.client = BleakClient(dev, timeout=15.0)
        await self.client.connect()
        # Subscribe proxy Data-Out so the node keeps the proxy link active; ignore RX.
        try:
            await self.client.start_notify(keys.UUID_PROXY_DATA_OUT, lambda *_: None)
        except Exception:
            pass

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

    # --- direct GATT config ---
    async def set_name(self, name: str) -> None:
        await self._w(keys.UUID_CHR_NAME, name.encode("utf-8"))

    async def _upload(self, uuid, start, payload, crc, on_progress) -> None:
        await self._w(uuid, start)
        chunk = self._chunk(3)  # DATA header = op + u16 offset
        off, n = 0, len(payload)
        while off < n:
            part = payload[off : off + chunk]
            await self._w(uuid, bytes([keys.FRAME_DATA]) + _le16(off) + part)
            off += len(part)
            if on_progress:
                on_progress(off, n)
        await self._w(uuid, bytes([keys.FRAME_END]) + struct.pack("<I", crc))

    async def upload_image(self, fb: bytes, on_progress=None) -> None:
        crc = zlib.crc32(fb) & 0xFFFFFFFF
        start = bytes([keys.FRAME_START]) + _le16(len(fb)) + _le16(176) + _le16(264)
        await self._upload(keys.UUID_CHR_IMAGE, start, fb, crc, on_progress)

    async def upload_image_slot(self, slot: int, fmt: int, payload: bytes, on_progress=None) -> None:
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        w, h = (176, 264) if fmt == keys.FMT_BW else (264, 176)
        start = bytes([keys.FRAME_START, slot, fmt]) + _le16(len(payload)) + _le16(w) + _le16(h)
        await self._upload(keys.UUID_CHR_IMGSLOT, start, payload, crc, on_progress)

    async def set_screen(self, idx: int, header: str, body: str, on_progress=None) -> None:
        h = header.encode("utf-8")[:47]
        b = body.encode("utf-8")
        await self._w(keys.UUID_CHR_SCREEN, bytes([keys.FRAME_START, idx, len(h)]) + h)
        chunk = self._chunk(3)
        off = 0
        while off < len(b):
            part = b[off : off + chunk]
            await self._w(keys.UUID_CHR_SCREEN, bytes([keys.FRAME_DATA]) + _le16(off) + part)
            off += len(part)
            if on_progress:
                on_progress(off, len(b))
        await self._w(keys.UUID_CHR_SCREEN, bytes([keys.FRAME_END]))

    async def display(self, kind: int, idx: int) -> None:
        await self._w(keys.UUID_CHR_DISPLAY, bytes([kind, idx]))

    # --- firmware OTA (signed MCUboot image -> slot1) ---
    async def ota_update(self, path: str, on_progress=None) -> None:
        with open(path, "rb") as f:
            data = f.read()
        crc = zlib.crc32(data) & 0xFFFFFFFF
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
        # END: the badge applies the swap and reboots ~1.2 s later, so the link
        # drops shortly after — a write/disconnect error here is expected/success.
        try:
            await self._w(keys.UUID_CHR_OTA, bytes([keys.FRAME_END]) + struct.pack("<I", crc))
        except Exception:
            pass

    # --- mesh proxy ---
    async def proxy_write(self, pdu: bytes) -> None:
        await self._w(keys.UUID_PROXY_DATA_IN, pdu, response=False)
