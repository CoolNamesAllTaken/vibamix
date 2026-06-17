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
  ../common/bl_state.c
)

add_subdirectory(../peripherals/epd epd)
add_subdirectory(../peripherals/qrcodegen qrcodegen)

# NORDIC SDK APP END
