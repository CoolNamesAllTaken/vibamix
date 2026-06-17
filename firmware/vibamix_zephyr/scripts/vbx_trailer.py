#!/usr/bin/env python3
"""Append a vibamix CRC trailer to a raw app image for the direct-XIP bootloader.

The bootloader (firmware/bootloader) and the OTA receiver (src/ota.c) locate a
32-byte trailer at image_base + image_len, check its magic, and CRC-verify the
preceding image_len bytes. This tool pads the raw `zephyr.bin` to a 16-byte
boundary, computes crc32 over the padded image, and appends the trailer struct
(must match `struct vbx_img_trailer` in firmware/common/vbx_img.h).

Usage:
    vbx_trailer.py <in.bin> <out.bin> [--version N]
"""
import argparse
import struct
import zlib

MAGIC = 0x58474D49        # 'IMGX' (little-endian bytes "IMGX")
TRAILER_SIZE = 32
HDR_VERSION = 1
ALIGN = 16


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile")
    ap.add_argument("outfile")
    ap.add_argument("--version", type=int, default=0,
                    help="monotonic image version (e.g. epoch seconds)")
    args = ap.parse_args()

    with open(args.infile, "rb") as f:
        img = f.read()

    # Pad the image so its length (and thus the trailer) is 16-byte aligned.
    if len(img) % ALIGN:
        img += b"\x00" * (ALIGN - (len(img) % ALIGN))

    image_len = len(img)
    crc = zlib.crc32(img) & 0xFFFFFFFF

    trailer = struct.pack(
        "<IIIIHH12s",
        MAGIC,
        image_len,
        crc,
        args.version & 0xFFFFFFFF,
        HDR_VERSION,
        0,                 # reserved
        b"\x00" * 12,      # pad
    )
    assert len(trailer) == TRAILER_SIZE

    with open(args.outfile, "wb") as f:
        f.write(img)
        f.write(trailer)

    print(f"{args.outfile}: image_len={image_len} crc32=0x{crc:08x} "
          f"version={args.version} total={image_len + TRAILER_SIZE}")


if __name__ == "__main__":
    main()
