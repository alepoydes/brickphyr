# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
list(APPEND DTS_ROOT ${BRICKPHYR_ROOT})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hub_drivers)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/hub_light.c ${BRICKPHYR_ROOT}/drivers/input_resistor_ladder.c
  ${BRICKPHYR_ROOT}/drivers/led_tlc5955.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
