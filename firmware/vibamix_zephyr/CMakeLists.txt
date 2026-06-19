#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(vibamix)

# Short commit hash of the firmware source tree, shown on the ALS screen so a
# flashed device can be matched back to the build that produced it.
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE APP_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE APP_GIT_HASH_RESULT
)
if(NOT APP_GIT_HASH_RESULT EQUAL 0)
  set(APP_GIT_HASH "unknown")
endif()
add_compile_definitions(APP_GIT_HASH="${APP_GIT_HASH}")

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
  src/LTR329ALS.cpp
  src/ambient_light_sensor.cpp
)

add_subdirectory(../peripherals/epd epd)
add_subdirectory(../peripherals/qrcodegen qrcodegen)

# NORDIC SDK APP END
