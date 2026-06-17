#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(vibamix)

# Shared headers + bl_state used by both the app and the bootloader.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)

# NORDIC SDK APP START
target_sources(app PRIVATE
  src/main.cpp
  src/GUI.cpp
  src/LEDStrip.cpp
  src/MeshNode.cpp
  src/mesh_model.c
  src/image_xfer.c
  src/app_config.c
  src/identity.c
  src/qr_screen.cpp
  src/config_gatt.c
  src/ConfigMode.cpp
  src/battery.c
  src/badge_store.c
  src/dither.c
  src/ota.c
  src/gateway_status.cpp
  ../common/bl_state.c
)

add_subdirectory(../peripherals/epd epd)
add_subdirectory(../peripherals/qrcodegen qrcodegen)

# --- OTA artifacts: auto-trailer this slot's image, and pack vibamix.ota --------
# A build only ever links one slot (CONFIG_FLASH_LOAD_OFFSET). As a POST_BUILD step
# (Zephyr's `extra_post_build_commands`, the same hook MCUboot signing uses) we
# append the CRC trailer to zephyr.bin -> slotA.bin/slotB.bin, then pack
# vibamix.ota once BOTH slot images exist. So `scripts/build_slots.sh` (which builds
# both slots into a shared dir) yields build/vibamix.ota with no extra steps, and a
# plain single build still auto-produces its trailered slotX.bin.
if(CONFIG_BUILD_OUTPUT_BIN AND DEFINED CONFIG_FLASH_LOAD_OFFSET)
  # Where the trailered slot bins + bundle go (build_slots.sh points this at one
  # shared dir for both slot builds; a plain build keeps them in its own build dir).
  set(VBX_ARTIFACTS_DIR "${CMAKE_BINARY_DIR}" CACHE PATH "OTA artifact output dir")
  # Monotonic image version; build_slots.sh passes one shared value for both slots.
  string(TIMESTAMP _vbx_ts "%s")
  set(VBX_VERSION "${_vbx_ts}" CACHE STRING "OTA image version (e.g. epoch seconds)")

  set(_vbx_bin     "${ZEPHYR_BINARY_DIR}/zephyr.bin")
  set(_vbx_scripts "${CMAKE_CURRENT_SOURCE_DIR}/scripts")

  # Which slot did we link? (offsets mirror slot_a/slot_b_partition in the board DTS.)
  math(EXPR _vbx_off "${CONFIG_FLASH_LOAD_OFFSET}")
  if(_vbx_off EQUAL 57344)         # 0xE000
    set(_vbx_slot "A")
  elseif(_vbx_off EQUAL 581632)    # 0x8E000
    set(_vbx_slot "B")
  else()
    set(_vbx_slot "")
  endif()

  if(_vbx_slot)
    set(_vbx_slotbin "${VBX_ARTIFACTS_DIR}/slot${_vbx_slot}.bin")
    # Ordered after zephyr.bin via a file-level DEPENDS (a POST_BUILD can't attach
    # to zephyr_final from this directory scope, and the app runs too late for the
    # extra_post_build_commands hook). After the bin exists it:
    #   1) appends the CRC trailer to this slot's raw image -> slotX.bin (the OUTPUT)
    #   2) packs vibamix.ota once BOTH slot images exist (byproduct; skips otherwise)
    add_custom_command(
      OUTPUT ${_vbx_slotbin}
      BYPRODUCTS ${VBX_ARTIFACTS_DIR}/vibamix.ota
      COMMAND ${PYTHON_EXECUTABLE} ${_vbx_scripts}/vbx_trailer.py
              ${_vbx_bin} ${_vbx_slotbin} --version ${VBX_VERSION}
      COMMAND ${CMAKE_COMMAND}
              -DA=${VBX_ARTIFACTS_DIR}/slotA.bin -DB=${VBX_ARTIFACTS_DIR}/slotB.bin
              -DOUT=${VBX_ARTIFACTS_DIR}/vibamix.ota -DPY=${PYTHON_EXECUTABLE}
              -DPACK=${_vbx_scripts}/pack_ota.py -DVER=${VBX_VERSION}
              -P ${_vbx_scripts}/pack_ota.cmake
      DEPENDS ${_vbx_bin}
      COMMENT "vibamix: trailer slot ${_vbx_slot} -> ${_vbx_slotbin}; pack vibamix.ota when both slots built"
      VERBATIM)
    add_custom_target(vbx_ota ALL DEPENDS ${_vbx_slotbin})
    message(STATUS "vibamix: slot ${_vbx_slot} -> ${_vbx_slotbin} (+ vibamix.ota when both slots built)")
  else()
    message(STATUS "vibamix: CONFIG_FLASH_LOAD_OFFSET=${CONFIG_FLASH_LOAD_OFFSET} is not a known slot; skipping OTA artifacts")
  endif()
endif()

# NORDIC SDK APP END
