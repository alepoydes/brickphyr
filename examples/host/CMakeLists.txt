# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(brickphyr_imu_fir_replay C)

set(CMSIS_DSP_DIR "" CACHE PATH "Path to the west CMSIS-DSP checkout")
if(NOT EXISTS "${CMSIS_DSP_DIR}/Source/FilteringFunctions/arm_fir_f32.c")
  message(FATAL_ERROR "Set CMSIS_DSP_DIR to the CMSIS-DSP module directory")
endif()

add_library(brickphyr_imu_fir SHARED
  imu_filter.c
  "${CMSIS_DSP_DIR}/Source/FilteringFunctions/arm_fir_f32.c"
)
target_include_directories(brickphyr_imu_fir PRIVATE "${CMSIS_DSP_DIR}/Include")
# CMSIS-DSP's documented desktop GCC mode. The F4 DSP profile leaves
# CONFIG_CMSIS_DSP_LOOPUNROLL disabled, so ARM_MATH_LOOPUNROLL is absent here.
target_compile_definitions(brickphyr_imu_fir PRIVATE __GNUC_PYTHON__ DISABLEFLOAT16)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(brickphyr_imu_fir PRIVATE -O2)
endif()
