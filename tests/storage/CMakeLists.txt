# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(CONF_FILE ${BRICKPHYR_ROOT}/prj.conf ${CMAKE_CURRENT_SOURCE_DIR}/prj.conf)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(settings_storage)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/storage.c ${BRICKPHYR_ROOT}/src/storage_mount.c
  ${BRICKPHYR_ROOT}/src/hub_power.c ${BRICKPHYR_ROOT}/src/configuration.c
  ${BRICKPHYR_ROOT}/src/ports/settings.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include ${BRICKPHYR_ROOT}/sdk/include)
