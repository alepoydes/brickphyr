# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)

list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND SYSCALL_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/include ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(brickphyr)

# Zephyr otherwise tracks its own Git index for both version headers.
# Use its application-version dependency hook, including in Git worktrees.
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --path-format=absolute
      --git-path index --git-path HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE revision_result OUTPUT_VARIABLE revision_dependencies
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(revision_result EQUAL 0)
    string(REPLACE "\n" ";" revision_dependencies "${revision_dependencies}")
    set_property(TARGET app_version_h PROPERTY APP_VERSION_DEPENDS ${revision_dependencies})
  endif()
endif()

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_BRICKPHYR_MATRIX app PRIVATE src/hub_matrix.c)
target_sources_ifdef(CONFIG_BRICKPHYR_TELEMETRY app PRIVATE src/telemetry.c)
target_include_directories(app PRIVATE sdk/include)

if(CONFIG_BRICKPHYR_NATIVE)
  set(BRICKPHYR_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
  include(sdk/native.cmake)
endif()

if(CONFIG_BOARD_SPIKE_PRIME_F4)
  if(CONFIG_BT_HCI_SETUP)
    target_sources(app PRIVATE drivers/ti_cc2564c.c src/hub_bluetooth.c src/host_ble.c)
    generate_inc_file_for_target(app
      ${WEST_TOPDIR}/modules/lib/ti-bt-service-packs/initscripts/TIInit_6.12.26.bts
      ${ZEPHYR_BINARY_DIR}/include/generated/ti_cc2564c_bts.inc)
  endif()
  target_sources(app PRIVATE src/host_usb.c src/host_transport.c src/host_protocol.c
    ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/src/cobs.c
    ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/src/protocol/pybricks.c
    src/hub.c src/hub_menu.c src/hub_light.c src/hub_power.c src/storage.c src/storage_mount.c
    drivers/input_resistor_ladder.c drivers/led_tlc5955.c)
  target_include_directories(app PRIVATE include ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
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
