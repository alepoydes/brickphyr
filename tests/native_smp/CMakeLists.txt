# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
set(CONF_FILE ../native/prj.conf prj.conf storage.conf)
list(APPEND DTS_ROOT ${BRICKPHYR_ROOT} ${BRICKPHYR_ROOT}/tests/ports)
list(APPEND SYSCALL_INCLUDE_DIRS ${BRICKPHYR_ROOT}/sdk/include ${BRICKPHYR_ROOT}/include)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(native_smp)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/pbio.cmake)
if(CONFIG_BRICKPHYR_MOTOR_CONTROL)
  include(${BRICKPHYR_ROOT}/cmake/motor_control.cmake)
endif()
if(NOT CONFIG_SHELL_BACKEND_SERIAL)
  target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/host_transport.c
    ${BRICKPHYR_ROOT}/src/host_protocol.c
    ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/src/protocol/pybricks.c)
  target_include_directories(app PRIVATE ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
endif()
include(${BRICKPHYR_ROOT}/sdk/native.cmake)
foreach(probe record record_event control)
  add_llext_target(${probe}_probe OUTPUT ${PROJECT_BINARY_DIR}/${probe}_probe.elf
    SOURCES ${BRICKPHYR_ROOT}/tests/native/${probe}.c)
  add_dependencies(${probe}_probe_llext_lib brickphyr_api)
  llext_compile_options(${probe}_probe -O2 -g0)
  llext_link_options(${probe}_probe -nostdlib -Wl,-T,${BRICKPHYR_ROOT}/sdk/include/brickphyr.ld.h)
  get_target_property(probe_target ${probe}_probe lib_target)
  target_link_libraries(${probe_target} PRIVATE c gcc)
  add_dependencies(app ${probe}_probe)
endforeach()
if(CONFIG_BRICKPHYR_PYTHON)
  include(${BRICKPHYR_ROOT}/tests/python/native.cmake)
endif()
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/hub_power.c ${BRICKPHYR_ROOT}/src/configuration.c)
target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/hub_battery.c ../native/battery_adc.c)
target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/hub_light.c ${BRICKPHYR_ROOT}/src/hub_matrix.c
  ${BRICKPHYR_ROOT}/drivers/led_tlc5955.c)
target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/hub_ports.c ${BRICKPHYR_ROOT}/src/ports/output.c ../ports/wiring.c)
target_sources_ifdef(CONFIG_BRICKPHYR_PORT_UART app PRIVATE pup_peer.c
  ${BRICKPHYR_ROOT}/src/ports/settings.c
  ${BRICKPHYR_ROOT}/src/ports/uart.c ${BRICKPHYR_ROOT}/src/ports/lump.c
  ${BRICKPHYR_ROOT}/src/ports/sensor.c)
target_include_directories(app PRIVATE ${WEST_TOPDIR}/pybricks-micropython/lib/lego)
target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/storage_mount.c)
target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/telemetry.c)
target_sources(app PRIVATE ../native/sound_output.c ${BRICKPHYR_ROOT}/src/hub_sound.c
  ${BRICKPHYR_ROOT}/src/sound/waveform.c)
if(CONFIG_BRICKPHYR_IMU)
  include(${BRICKPHYR_ROOT}/cmake/hub_imu.cmake)
  target_sources(app PRIVATE imu.c)
endif()

target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/hub_charger.c
  ${BRICKPHYR_ROOT}/drivers/input_resistor_ladder.c)
