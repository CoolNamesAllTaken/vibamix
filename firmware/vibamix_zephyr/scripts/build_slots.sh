#!/usr/bin/env bash
#
# Build the vibamix app for BOTH direct-XIP slots (A and B). Each build's
# CMakeLists POST_BUILD step appends the CRC trailer (-> build/slotA.bin /
# slotB.bin) and packs build/vibamix.ota once both slot images exist (see the
# OTA artifacts block in CMakeLists.txt). The two binaries are identical source,
# linked at different offsets — always build both so they stay in lockstep.
#
# We just drive the two builds with a shared output dir + version so the trailer
# + bundle land together. Requires the NCS toolchain env on PATH (see the
# build-env memory). Run from the firmware/vibamix_zephyr directory.
set -euo pipefail

BOARD=vibamix_xiao/nrf54l15/cpuapp
APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$APP_DIR/build"
SLOTB_OVERLAY="$APP_DIR/boards/pants_for_birds/vibamix_xiao/slotb.overlay"

# One monotonic version for this build (epoch seconds), shared by both slots.
VERSION="${VBX_VERSION:-$(date +%s)}"

# Clear stale artifacts so a half-build can't leave a mismatched bundle.
rm -f "$OUT/slotA.bin" "$OUT/slotB.bin" "$OUT/vibamix.ota"

# --no-sysbuild is REQUIRED: the direct-XIP slots come from the DTS code-partition
# (CONFIG_USE_DT_CODE_PARTITION); sysbuild's Partition Manager would ignore that and
# mislink the app. It also lets -D… reach the app image directly (so the artifact
# dir + version below take effect). See the build-env memory.
echo "=== Building slot A (version $VERSION) ==="
west build -p always -b "$BOARD" --no-sysbuild -d "$OUT/slotA" "$APP_DIR" -- \
	-DBOARD_ROOT="$APP_DIR" -DVBX_ARTIFACTS_DIR="$OUT" -DVBX_VERSION="$VERSION"

echo "=== Building slot B (version $VERSION) ==="
west build -p always -b "$BOARD" --no-sysbuild -d "$OUT/slotB" "$APP_DIR" -- \
	-DBOARD_ROOT="$APP_DIR" -DEXTRA_DTC_OVERLAY_FILE="$SLOTB_OVERLAY" \
	-DVBX_ARTIFACTS_DIR="$OUT" -DVBX_VERSION="$VERSION"

echo
echo "Done (trailers + bundle written by the per-slot CMake POST_BUILD):"
echo "  $OUT/slotA.bin   (flash at slot A over SWD, or OTA when A is inactive)"
echo "  $OUT/slotB.bin   (OTA when B is inactive)"
echo "  $OUT/vibamix.ota (single bundle for badgectl OTA — picks the right slot)"
