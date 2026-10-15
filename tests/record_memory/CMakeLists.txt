# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(record_memory)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/application_record.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include ${BRICKPHYR_ROOT}/sdk/include)
