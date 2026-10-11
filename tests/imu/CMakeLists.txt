# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
list(APPEND DTS_ROOT ${BRICKPHYR_ROOT})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hub_imu)
include(${BRICKPHYR_ROOT}/cmake/hub_imu.cmake)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/telemetry.c
  ${BRICKPHYR_ROOT}/src/hub_power.c)
if(IMU_INIT_FAILURE)
  target_compile_definitions(app PRIVATE IMU_INIT_FAILURE)
endif()
