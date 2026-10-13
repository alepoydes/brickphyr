# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(CONF_FILE ${BRICKPHYR_ROOT}/prj.conf ${CMAKE_CURRENT_SOURCE_DIR}/prj.conf)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cc2640_host)
include(${BRICKPHYR_ROOT}/cmake/pbio.cmake)
include(${BRICKPHYR_ROOT}/cmake/cc2640.cmake)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/host_cc2640.c
  ${BRICKPHYR_ROOT}/src/host_ble_session.c ${BRICKPHYR_ROOT}/src/host_transport.c
  ${BRICKPHYR_ROOT}/src/host_protocol.c ${BRICKPHYR_ROOT}/src/hub_power.c
  ${BRICKPHYR_ROOT}/src/configuration.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include ${BRICKPHYR_ROOT}/sdk/include)
