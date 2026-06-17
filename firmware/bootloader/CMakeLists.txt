#
# Vibamix first-stage bootloader (custom direct-XIP A/B, no MCUboot).
#
# Tiny standalone Zephyr app linked into `bootloader_partition` at 0x0. It picks
# a verified app slot (A/B) from bl_state, CRC-checks its trailer, arms the
# watchdog for trial images, and chain-loads it. Build/flash once over SWD:
#   west build -b vibamix_xiao/nrf54l15/cpuapp -d build/bl firmware/bootloader
#   west flash -d build/bl
#
cmake_minimum_required(VERSION 3.20.0)

# The board lives in the app tree; reuse it.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vibamix_zephyr)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(vbx_bootloader)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_sources(app PRIVATE
  src/main.c
  ../common/bl_state.c
)
