# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
list(APPEND DTS_ROOT ${BRICKPHYR_ROOT})
set(CONF_FILE ${BRICKPHYR_ROOT}/prj.conf
  ${BRICKPHYR_ROOT}/tests/host/prj.conf
  ${BRICKPHYR_ROOT}/tests/storage/prj.conf
  ${CMAKE_CURRENT_SOURCE_DIR}/prj.conf)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(management_interop)
target_sources(app PRIVATE main.c
  ${BRICKPHYR_ROOT}/src/host_transport.c
  ${BRICKPHYR_ROOT}/src/host_protocol.c
  ${BRICKPHYR_ROOT}/src/hub_power.c ${BRICKPHYR_ROOT}/src/configuration.c
  ${BRICKPHYR_ROOT}/src/telemetry.c
  ${BRICKPHYR_ROOT}/src/hub_matrix.c
  ${BRICKPHYR_ROOT}/drivers/led_tlc5955.c
  ${BRICKPHYR_ROOT}/src/storage.c ${BRICKPHYR_ROOT}/src/storage_mount.c
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/src/cobs.c
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/src/protocol/pybricks.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include ${BRICKPHYR_ROOT}/sdk/include
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
