# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
list(APPEND DTS_ROOT ${BRICKPHYR_ROOT} ${BRICKPHYR_ROOT}/tests/ports)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lump)
target_sources(app PRIVATE main.c ../ports/wiring.c
  ${BRICKPHYR_ROOT}/src/ports/lump.c ${BRICKPHYR_ROOT}/src/ports/uart.c
  ${BRICKPHYR_ROOT}/src/ports/sensor.c
  ${BRICKPHYR_ROOT}/src/hub_ports.c ${BRICKPHYR_ROOT}/src/ports/output.c ${BRICKPHYR_ROOT}/src/telemetry.c
  ${BRICKPHYR_ROOT}/src/hub_power.c ${BRICKPHYR_ROOT}/src/configuration.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/src/ports
  ${BRICKPHYR_ROOT}/tests/ports ${BRICKPHYR_ROOT}/include
  ${BRICKPHYR_ROOT}/sdk/include ${WEST_TOPDIR}/pybricks-micropython/lib/lego)
target_include_directories(app PRIVATE ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
include(${BRICKPHYR_ROOT}/cmake/pbio.cmake)
