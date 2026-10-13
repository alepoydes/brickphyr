# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(CONF_FILE ${BRICKPHYR_ROOT}/prj.conf ${CMAKE_CURRENT_SOURCE_DIR}/prj.conf)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(host_transport)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/pbio.cmake)
if(CONFIG_SERIAL)
  target_sources(app PRIVATE main.c)
else()
  target_sources(app PRIVATE ble_only.c ${BRICKPHYR_ROOT}/src/host_ble_session.c)
endif()
target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/host_transport.c
  ${BRICKPHYR_ROOT}/src/hub_power.c ${BRICKPHYR_ROOT}/src/configuration.c
  ${BRICKPHYR_ROOT}/src/host_protocol.c
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/src/protocol/pybricks.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include ${BRICKPHYR_ROOT}/sdk/include
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
