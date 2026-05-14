#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(vibamix)

# NORDIC SDK APP START
target_sources(app PRIVATE
  src/main.cpp
)

add_subdirectory(../peripherals/epd epd)

# NORDIC SDK APP END
