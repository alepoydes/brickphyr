# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(imu_processing)

set(pbio ${WEST_TOPDIR}/pybricks-micropython/lib/pbio)
target_sources(app PRIVATE main.c calculation.c
  ${BRICKPHYR_ROOT}/src/imu/processing.c ${BRICKPHYR_ROOT}/src/imu/stationary.c
  ${pbio}/src/geometry.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/src/imu
  ${BRICKPHYR_ROOT}/drivers/pybricks ${pbio}/include)
