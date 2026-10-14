# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
# The portable SDK supplies the target's ARM architecture, float ABI and libc.
set(CMAKE_C_COMPILER "$ENV{ZEPHYR_SDK_INSTALL_DIR}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(brickphyr_examples C)
if(NOT LLEXT_EDK_INSTALL_DIR)
  set(LLEXT_EDK_INSTALL_DIR "$ENV{LLEXT_EDK_INSTALL_DIR}")
endif()
include(${LLEXT_EDK_INSTALL_DIR}/cmake.cflags)
# Picolibc specs also inject linker script and section-GC options; the final
# relocatable link selects its static archives explicitly below.
set(application_link_flags ${LLEXT_CFLAGS})
list(FILTER application_link_flags EXCLUDE REGEX "^(-specs=|-I)")
file(WRITE ${PROJECT_BINARY_DIR}/application.ld.in "#include <brickphyr.ld.h>\n")
add_custom_command(OUTPUT application.ld
  COMMAND ${CMAKE_C_COMPILER} ${LLEXT_CFLAGS} -E -P -MMD -MF application.ld.d
    -x c application.ld.in -o application.ld
  DEPENDS application.ld.in ${LLEXT_EDK_INSTALL_DIR}/cmake.cflags
  DEPFILE application.ld.d
  VERBATIM COMMAND_EXPAND_LISTS)
add_custom_target(application_layout DEPENDS application.ld)
set(native_examples hello counter spin fault matrix imu control_frame control_group control_group_coast control_record sound motor)
set(dsp_library ${LLEXT_EDK_INSTALL_DIR}/lib/libcmsis-dsp.a)
if(EXISTS ${dsp_library})
  list(APPEND native_examples imu_filter)
endif()
foreach(example ${native_examples})
  if(example STREQUAL "control_group_coast")
    add_executable(${example} control_group.c)
    target_compile_definitions(${example} PRIVATE BP_GROUP_COAST=1)
  else()
    add_executable(${example} ${example}.c)
  endif()
  set_target_properties(${example} PROPERTIES SUFFIX .elf
    LINK_DEPENDS ${PROJECT_BINARY_DIR}/application.ld)
  target_compile_options(${example} PRIVATE ${LLEXT_CFLAGS} -O2 -g0)
  # Select the toolchain's compact libc archive; application code stays at -O2.
  target_link_options(${example} PRIVATE ${application_link_flags} -Os -nostdlib
    -r -Wl,--strip-debug -T${PROJECT_BINARY_DIR}/application.ld)
  # Compiler-emitted memory operations execute in the application's domain.
  # Resolve them from the compiler's static C library, never privileged imports.
  if(example STREQUAL "imu_filter")
    target_link_libraries(${example} PRIVATE ${dsp_library})
    # This example prints integers only, including the full long-long range.
    # This is Picolibc's integer formatter selection from picolibc.specs.
    target_link_options(${example} PRIVATE "-Wl,--defsym,vfprintf=__l_vfprintf")
  endif()
  target_link_libraries(${example} PRIVATE c gcc)
  add_dependencies(${example} application_layout)
endforeach()
