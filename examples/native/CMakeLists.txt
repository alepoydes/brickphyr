# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
# The exported SDK supplies the board's ARM architecture and float ABI.
set(CMAKE_C_COMPILER "$ENV{ZEPHYR_SDK_INSTALL_DIR}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(brickphyr_examples C)
if(NOT LLEXT_EDK_INSTALL_DIR)
  set(LLEXT_EDK_INSTALL_DIR "$ENV{LLEXT_EDK_INSTALL_DIR}")
endif()
include(${LLEXT_EDK_INSTALL_DIR}/cmake.cflags)
# Keep compile-only library specs out of the application link.
set(application_link_flags ${LLEXT_CFLAGS})
list(FILTER application_link_flags EXCLUDE REGEX "^-specs=")
file(WRITE ${PROJECT_BINARY_DIR}/application.ld.in "#include <brickphyr.ld.h>\n")
add_custom_command(OUTPUT application.ld
  COMMAND ${CMAKE_C_COMPILER} ${LLEXT_CFLAGS} -E -P -MMD -MF application.ld.d
    -x c application.ld.in -o application.ld
  DEPENDS application.ld.in ${LLEXT_EDK_INSTALL_DIR}/cmake.cflags
  DEPFILE application.ld.d
  VERBATIM COMMAND_EXPAND_LISTS)
add_custom_target(application_layout DEPENDS application.ld)
foreach(example hello counter spin fault matrix imu sound motor)
  add_executable(${example} ${example}.c)
  set_target_properties(${example} PROPERTIES SUFFIX .elf
    LINK_DEPENDS ${PROJECT_BINARY_DIR}/application.ld)
  target_compile_options(${example} PRIVATE ${LLEXT_CFLAGS} -O2 -g0)
  target_link_options(${example} PRIVATE ${application_link_flags} -nostdlib
    -r -T${PROJECT_BINARY_DIR}/application.ld)
  # Compiler-emitted memory operations execute in the application's domain.
  # Resolve them from the SDK's static C library, never privileged imports.
  target_link_libraries(${example} PRIVATE c gcc)
  add_dependencies(${example} application_layout)
endforeach()
