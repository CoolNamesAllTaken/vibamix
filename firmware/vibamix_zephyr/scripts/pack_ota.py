#!/usr/bin/env python3
"""Pack the two direct-XIP slot images into one `.ota` container.

A vibamix app is built twice (linked at each slot's offset) and each raw image
gets a 32-byte CRC trailer (vbx_trailer.py), producing slotA.bin / slotB.bin.
This tool bundles both into a single `.ota` file with a small directory header,
so the host (badgectl) ships one artifact: it reads the badge's running slot over
BLE (OTA-status f0de000A), then extracts and streams the image for the *inactive*
slot. See firmware/vibamix_zephyr/docs/ble-config-api.md §10.

Container layout (little-endian):
    offset 0   4   magic = "VOTA"
    offset 4   u16 format_version (= 1)
    offset 6   u16 slot_count
    offset 8   u32 app_version (shared by both slots)
    offset 12  u32 reserved (0)
    offset 16  directory[slot_count], 12 bytes each:
                   u8  slot (0=A, 1=B)
                   u8  reserved[3]
                   u32 offset   (absolute byte offset of this slot's image)
                   u32 length   (image length incl. its 32-byte trailer)
    then each slot image payload at its offset.

Usage:
    pack_ota.py <slotA.bin> <slotB.bin> -o <out.ota> [--version N]
"""
import argparse
import struct

MAGIC = b"VOTA"
FORMAT_VERSION = 1
HEADER_SIZE = 16
DIRENT_SIZE = 12

TRAILER_SIZE = 32
TRAILER_MAGIC = 0x58474D49  # 'IMGX' — must match scripts/vbx_trailer.py / vbx_img.h


def slot_version(img: bytes) -> int | None:
    """Return the build version from a slot image's CRC trailer, or None if the
    trailer is missing/invalid (i.e. the input wasn't run through vbx_trailer.py)."""
    if len(img) < TRAILER_SIZE:
        return None
    magic, _image_len, _crc, version = struct.unpack_from("<IIII", img, len(img) - TRAILER_SIZE)
    if magic != TRAILER_MAGIC:
        return None
    return version


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("slotA", help="slot A image (raw + CRC trailer)")
    ap.add_argument("slotB", help="slot B image (raw + CRC trailer)")
    ap.add_argument("-o", "--out", required=True, help="output .ota file")
    ap.add_argument("--version", type=int, default=None,
                    help="override app version (default: read from the slot trailers)")
    args = ap.parse_args()

    images = []  # (slot_index, data)
    for slot, path in enumerate((args.slotA, args.slotB)):
        with open(path, "rb") as f:
            data = f.read()
        if slot_version(data) is None:
            raise SystemExit(f"{path}: missing/invalid CRC trailer (run vbx_trailer.py first)")
        images.append((slot, data))

    versions = {slot_version(d) for _, d in images}
    if args.version is not None:
        version = args.version
    elif len(versions) == 1:
        version = versions.pop()
    else:
        raise SystemExit(f"slot images have different versions {sorted(versions)}; pass --version")

    slot_count = len(images)
    payload_off = HEADER_SIZE + DIRENT_SIZE * slot_count
    directory = []
    off = payload_off
    for slot, data in images:
        directory.append((slot, off, len(data)))
        off += len(data)

    with open(args.out, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<HHII", FORMAT_VERSION, slot_count, version & 0xFFFFFFFF, 0))
        for slot, offset, length in directory:
            f.write(struct.pack("<BxxxII", slot, offset, length))
        for _, data in images:
            f.write(data)

    print(f"{args.out}: {slot_count} slots, version {version}, {off} bytes total")
    for slot, offset, length in directory:
        print(f"  slot {slot}: {length} bytes @ {offset}")


if __name__ == "__main__":
    main()
