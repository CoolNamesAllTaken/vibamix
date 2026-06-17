#!/usr/bin/env bash
#
# Build the vibamix app for BOTH direct-XIP slots (A and B) and append a CRC
# trailer to each, producing build/slotA.bin and build/slotB.bin ready to flash
# (slot A, over SWD) or OTA (the inactive slot). The two binaries are identical
# source, linked at different offsets — always build both so they stay in lockstep.
#
# Requires the NCS toolchain env on PATH (see the build-env memory). Run from the
# firmware/vibamix_zephyr directory.
set -euo pipefail

BOARD=vibamix_xiao/nrf54l15/cpuapp
APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$APP_DIR/build"
SLOTB_OVERLAY="$APP_DIR/boards/pants_for_birds/vibamix_xiao/slotb.overlay"
TRAILER="$APP_DIR/scripts/vbx_trailer.py"

# One monotonic version for this build (epoch seconds), shared by both slots.
VERSION="${VBX_VERSION:-$(date +%s)}"

echo "=== Building slot A (version $VERSION) ==="
west build -p always -b "$BOARD" -d "$OUT/slotA" "$APP_DIR" -- -DBOARD_ROOT="$APP_DIR"

echo "=== Building slot B (version $VERSION) ==="
west build -p always -b "$BOARD" -d "$OUT/slotB" "$APP_DIR" -- \
	-DBOARD_ROOT="$APP_DIR" -DEXTRA_DTC_OVERLAY_FILE="$SLOTB_OVERLAY"

echo "=== Appending CRC trailers ==="
python3 "$TRAILER" "$OUT/slotA/zephyr/zephyr.bin" "$OUT/slotA.bin" --version "$VERSION"
python3 "$TRAILER" "$OUT/slotB/zephyr/zephyr.bin" "$OUT/slotB.bin" --version "$VERSION"

echo
echo "Done:"
echo "  $OUT/slotA.bin  (flash at slot A over SWD, or OTA when A is inactive)"
echo "  $OUT/slotB.bin  (OTA when B is inactive)"
