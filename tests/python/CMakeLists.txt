# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
list(APPEND SYSCALL_INCLUDE_DIRS ${BRICKPHYR_ROOT}/sdk/include)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(python_runtime)
add_subdirectory(${BRICKPHYR_ROOT}/src/python python)
target_sources(app PRIVATE main.c)
if(CONFIG_FPU)
  target_sources(app PRIVATE fpu.c)
  target_include_directories(app PRIVATE ${WEST_TOPDIR}/pybricks-micropython/micropython
    ${BRICKPHYR_ROOT}/src/python
    ${CMAKE_BINARY_DIR})
endif()
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/calculation.mpy
  COMMAND ${CMAKE_BINARY_DIR}/mpy-cross/mpy-cross -s calculation.py
    -o ${CMAKE_CURRENT_BINARY_DIR}/calculation.mpy ${CMAKE_CURRENT_SOURCE_DIR}/calculation.py
  DEPENDS mpy_cross calculation.py VERBATIM)
generate_inc_file_for_target(app ${CMAKE_CURRENT_BINARY_DIR}/calculation.mpy
  ${ZEPHYR_BINARY_DIR}/include/generated/calculation.inc)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/reference.mpy
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/compile_reference.py
    ${CMAKE_CURRENT_SOURCE_DIR}/calculation.py ${CMAKE_CURRENT_BINARY_DIR}/reference.mpy
  DEPENDS calculation.py compile_reference.py ${BRICKPHYR_ROOT}/uv.lock VERBATIM)
generate_inc_file_for_target(app ${CMAKE_CURRENT_BINARY_DIR}/reference.mpy
  ${ZEPHYR_BINARY_DIR}/include/generated/reference.inc)
