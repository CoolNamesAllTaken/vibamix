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
BL_DIR="$(cd "$APP_DIR/.." && pwd)/bootloader"  # standalone first-stage BL app

# One monotonic version for this build (epoch seconds), shared by both slots.
VERSION="${VBX_VERSION:-$(date +%s)}"

# Pristine by default (safe, unambiguous). Set VBX_PRISTINE=0 for fast incremental
# rebuilds — e.g. the VS Code default build task does, so Ctrl/Cmd+Shift+B is quick.
# Set VBX_SKIP_BL=1 to skip the bootloader build (fast OTA-only iterations — the BL
# rarely changes and isn't needed for an OTA bundle).
PRISTINE_ARGS=(-p always)
if [ "${VBX_PRISTINE:-1}" = "0" ]; then
	PRISTINE_ARGS=()
fi

# Clear stale artifacts so a half-build can't leave a mismatched bundle.
rm -f "$OUT/slotA.bin" "$OUT/slotB.bin" "$OUT/vibamix.ota"

# --no-sysbuild is REQUIRED: the direct-XIP slots come from the DTS code-partition
# (CONFIG_USE_DT_CODE_PARTITION); sysbuild's Partition Manager would ignore that and
# mislink the app. It also lets -D… reach the app image directly (so the artifact
# dir + version below take effect). See the build-env memory.

# Bootloader first (fail fast if it's broken). It's a separate standalone Zephyr app
# with no version/artifact knobs; its CMakeLists self-appends BOARD_ROOT. Output
# build/bl/zephyr/zephyr.hex is what badgectl's Flash tab expects (flashconfig.py).
if [ "${VBX_SKIP_BL:-0}" != "1" ]; then
	echo "=== Building bootloader ==="
	west build ${PRISTINE_ARGS[@]+"${PRISTINE_ARGS[@]}"} -b "$BOARD" --no-sysbuild -d "$OUT/bl" "$BL_DIR" -- \
		-DBOARD_ROOT="$APP_DIR"
fi

echo "=== Building slot A (version $VERSION) ==="
west build ${PRISTINE_ARGS[@]+"${PRISTINE_ARGS[@]}"} -b "$BOARD" --no-sysbuild -d "$OUT/slotA" "$APP_DIR" -- \
	-DBOARD_ROOT="$APP_DIR" -DVBX_ARTIFACTS_DIR="$OUT" -DVBX_VERSION="$VERSION"

echo "=== Building slot B (version $VERSION) ==="
west build ${PRISTINE_ARGS[@]+"${PRISTINE_ARGS[@]}"} -b "$BOARD" --no-sysbuild -d "$OUT/slotB" "$APP_DIR" -- \
	-DBOARD_ROOT="$APP_DIR" -DEXTRA_DTC_OVERLAY_FILE="$SLOTB_OVERLAY" \
	-DVBX_ARTIFACTS_DIR="$OUT" -DVBX_VERSION="$VERSION"

echo
echo "Done (trailers + bundle written by the per-slot CMake POST_BUILD):"
if [ "${VBX_SKIP_BL:-0}" != "1" ]; then
	echo "  $OUT/bl/zephyr/zephyr.hex  (flash once over SWD at 0x0; badgectl Flash tab)"
fi
echo "  $OUT/slotA.bin   (flash at slot A over SWD, or OTA when A is inactive)"
echo "  $OUT/slotB.bin   (OTA when B is inactive)"
echo "  $OUT/vibamix.ota (single bundle for badgectl OTA — picks the right slot)"
