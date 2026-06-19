"""Convert image files into the badge's framebuffer / 2-bit grayscale formats.

Packing matches the firmware exactly:
  - 1-bit B/W: panel-native 5808 bytes, idx=(263-dx)*22+(dy>>3), mask=0x80>>(dy&7),
    bit 1=white/0=black (see docs/ble-config-api.md §6 and dither.c).
  - 2-bit gray: 11616 bytes, row-major pixel_index=dy*264+dx, 4 px/byte MSB-first,
    level 0=black..3=white (firmware dithers this to 1bpp on display).
Author canvas is landscape 264 (wide) x 176 (tall).
"""

from __future__ import annotations

from PIL import Image

CANVAS_W = 264
CANVAS_H = 176
BW_BYTES = 5808
GRAY2_BYTES = 11616


def _load_landscape(path: str) -> Image.Image:
    img = Image.open(path).convert("L")
    return img.resize((CANVAS_W, CANVAS_H))


def to_bw(path: str, dither: bool = True) -> bytes:
    """5808-byte 1bpp panel framebuffer (final B/W — host dithers)."""
    g = _load_landscape(path)
    mode = Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE
    bw = g.convert("1", dither=mode)  # 0=black, 255=white
    px = bw.load()

    buf = bytearray(b"\xff" * BW_BYTES)  # all white
    for dx in range(CANVAS_W):
        for dy in range(CANVAS_H):
            if px[dx, dy] == 0:  # black
                idx = (CANVAS_W - 1 - dx) * 22 + (dy >> 3)
                buf[idx] &= ~(0x80 >> (dy & 7)) & 0xFF
    return bytes(buf)


def to_gray2(path: str) -> bytes:
    """11616-byte 2-bit grayscale (4 levels; firmware dithers to B/W on display)."""
    g = _load_landscape(path)
    px = g.load()

    buf = bytearray(GRAY2_BYTES)
    for dy in range(CANVAS_H):
        for dx in range(CANVAS_W):
            level = (px[dx, dy] * 3 + 127) // 255  # 0..3, 0=black 3=white
            pi = dy * CANVAS_W + dx
            shift = (3 - (pi & 3)) * 2
            buf[pi >> 2] |= level << shift
    return bytes(buf)


def unpack_preview(buf: bytes, fmt: str) -> Image.Image:
    """Reconstruct what the badge will show, as an 'L' image (for preview)."""
    img = Image.new("L", (CANVAS_W, CANVAS_H), 255)
    px = img.load()
    if fmt == "bw":
        for dx in range(CANVAS_W):
            for dy in range(CANVAS_H):
                idx = (CANVAS_W - 1 - dx) * 22 + (dy >> 3)
                white = buf[idx] & (0x80 >> (dy & 7))
                px[dx, dy] = 255 if white else 0
    else:  # gray2
        for dy in range(CANVAS_H):
            for dx in range(CANVAS_W):
                pi = dy * CANVAS_W + dx
                shift = (3 - (pi & 3)) * 2
                level = (buf[pi >> 2] >> shift) & 0x3
                px[dx, dy] = level * 85
    return img
