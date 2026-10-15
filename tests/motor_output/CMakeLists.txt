# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
list(APPEND DTS_ROOT ${BRICKPHYR_ROOT} ${BRICKPHYR_ROOT}/tests/ports)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(motor_output)
target_sources(app PRIVATE main.c ../ports/wiring.c ${BRICKPHYR_ROOT}/src/ports/output.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/src/ports
  ${BRICKPHYR_ROOT}/tests/ports ${BRICKPHYR_ROOT}/sdk/include ${BRICKPHYR_ROOT}/include)
