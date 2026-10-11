# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(motor_control)
include(${BRICKPHYR_ROOT}/cmake/motor_control.cmake)
target_sources(app PRIVATE main.c)
