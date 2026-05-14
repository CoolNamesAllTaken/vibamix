#!/bin/bash

# Command line options documented here: https://kb.segger.com/J-Link_GDB_Server#Command_line_options

jlinkgdbserver -device NRF54L15_M33 -endian little -speed 4000 -if SWD -port 2331