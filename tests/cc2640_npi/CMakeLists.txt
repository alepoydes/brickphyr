# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
list(APPEND DTS_ROOT ${BRICKPHYR_ROOT})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cc2640_npi)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/drivers/cc2640_npi.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include)
