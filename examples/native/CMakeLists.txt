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
set(native_examples hello counter spin fault matrix imu control_frame control_group control_group_coast control_actuation control_actuation_coast control_record control_sample control_loop control_background sound motor)
set(dsp_library ${LLEXT_EDK_INSTALL_DIR}/lib/libcmsis-dsp.a)
if(EXISTS ${dsp_library})
  list(APPEND native_examples imu_filter imu_record)
endif()
foreach(example ${native_examples})
  if(example MATCHES "^control_(group|actuation)_coast$")
    string(REGEX REPLACE "_coast$" "" example_source ${example})
    add_executable(${example} ${example_source}.c)
    target_compile_definitions(${example} PRIVATE BP_GROUP_COAST=1)
  elseif(example STREQUAL "control_sample")
    add_executable(${example} control_record.c)
    target_compile_definitions(${example} PRIVATE BP_CONTROL_IMU=1)
  elseif(example STREQUAL "imu_record")
    add_executable(${example} imu_filter.c)
    target_compile_definitions(${example} PRIVATE BP_IMU_RECORD=1)
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
  if(example MATCHES "^imu_(filter|record)$")
    target_link_libraries(${example} PRIVATE ${dsp_library})
    # This example prints integers only, including the full long-long range.
    # This is Picolibc's integer formatter selection from picolibc.specs.
    target_link_options(${example} PRIVATE "-Wl,--defsym,vfprintf=__l_vfprintf")
  endif()
  target_link_libraries(${example} PRIVATE c gcc)
  add_dependencies(${example} application_layout)
endforeach()

# Embed a complete controller image in the primary ELF. LLEXT then loads it into
# its own domain; the embedded source remains part of the primary's read-only data.
add_custom_command(OUTPUT controller_image.o
  COMMAND "$ENV{ZEPHYR_SDK_INSTALL_DIR}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-objcopy"
    -I binary -O elf32-littlearm -B arm
    --set-section-alignment .data=4
    --rename-section .data=.rodata,alloc,load,readonly,data,contents
    --redefine-sym _binary_control_loop_elf_start=controller_image
    --redefine-sym _binary_control_loop_elf_end=controller_end
    --strip-symbol _binary_control_loop_elf_size
    control_loop.elf controller_image.o
  DEPENDS control_loop
  VERBATIM)
target_sources(control_background PRIVATE ${PROJECT_BINARY_DIR}/controller_image.o)
