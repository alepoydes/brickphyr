# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(brickphyr)

target_sources(app PRIVATE src/main.c)

if(CONFIG_BOARD_SPIKE_PRIME_F4)
  add_custom_target(firmware_package
    COMMAND "${PYTHON_EXECUTABLE}" -m tools.package_firmware
            "${ZEPHYR_BINARY_DIR}" --version "${APP_VERSION_STRING}"
            --sdk "${ZEPHYR_SDK_INSTALL_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS zephyr_final
    USES_TERMINAL
    VERBATIM
  )
endif()
