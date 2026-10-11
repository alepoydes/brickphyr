# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
list(APPEND DTS_ROOT ${BRICKPHYR_ROOT})
list(APPEND SYSCALL_INCLUDE_DIRS ${BRICKPHYR_ROOT}/sdk/include ${BRICKPHYR_ROOT}/include)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(native_lifecycle)
include(${BRICKPHYR_ROOT}/sdk/native.cmake)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/hub_power.c)
target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/hub_matrix.c
  ${BRICKPHYR_ROOT}/drivers/led_tlc5955.c)
target_include_directories(app PRIVATE ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
