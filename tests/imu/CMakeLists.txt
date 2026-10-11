# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
set(CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/prj.conf ${BRICKPHYR_ROOT}/tests/storage/prj.conf)
list(APPEND DTS_ROOT ${BRICKPHYR_ROOT})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hub_imu)
include(${BRICKPHYR_ROOT}/cmake/hub_imu.cmake)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/telemetry.c
  ${BRICKPHYR_ROOT}/src/hub_power.c ${BRICKPHYR_ROOT}/src/configuration.c
  ${BRICKPHYR_ROOT}/src/storage.c ${BRICKPHYR_ROOT}/src/storage_mount.c)
if(IMU_INIT_FAILURE)
  target_compile_definitions(app PRIVATE IMU_INIT_FAILURE)
endif()
