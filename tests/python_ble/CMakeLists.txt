# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
set(CONF_FILE ../native/prj.conf prj.conf)
list(APPEND SYSCALL_INCLUDE_DIRS ${BRICKPHYR_ROOT}/sdk/include ${BRICKPHYR_ROOT}/include)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(python_ble)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/pbio.cmake)
include(${BRICKPHYR_ROOT}/sdk/native.cmake)
target_sources_ifdef(CONFIG_BRICKPHYR_APP_STORAGE app PRIVATE ${BRICKPHYR_ROOT}/src/storage_mount.c)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/host_transport.c
	${BRICKPHYR_ROOT}/drivers/ti_cc2564c.c
  ${BRICKPHYR_ROOT}/src/host_protocol.c ${BRICKPHYR_ROOT}/src/host_ble.c
  ${BRICKPHYR_ROOT}/src/hub_power.c ${BRICKPHYR_ROOT}/src/configuration.c
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/src/protocol/pybricks.c)
target_include_directories(app PRIVATE ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
generate_inc_file_for_target(app
  ${WEST_TOPDIR}/modules/lib/ti-bt-service-packs/initscripts/TIInit_6.12.26.bts
  ${ZEPHYR_BINARY_DIR}/include/generated/ti_cc2564c_bts.inc)
zephyr_ld_options(-Wl,--wrap=bt_enable -Wl,--wrap=bt_disable
  -Wl,--wrap=bt_hci_cmd_send_sync
  -Wl,--wrap=bt_le_adv_start -Wl,--wrap=bt_le_adv_stop
  -Wl,--wrap=bt_le_scan_start -Wl,--wrap=bt_le_scan_stop
  -Wl,--wrap=bt_conn_ref -Wl,--wrap=bt_conn_unref -Wl,--wrap=bt_conn_disconnect
  -Wl,--wrap=bt_conn_get_info
  -Wl,--wrap=bt_conn_le_create -Wl,--wrap=bt_conn_set_security
  -Wl,--wrap=bt_gatt_discover -Wl,--wrap=bt_gatt_subscribe -Wl,--wrap=bt_gatt_write
  -Wl,--wrap=bt_gatt_is_subscribed -Wl,--wrap=bt_gatt_notify_cb -Wl,--wrap=bt_gatt_get_mtu)
set(programs echo spin flood native messaging radio lwp3 wireless)
if(CONFIG_BRICKPHYR_PYBRICKS_SLOTS GREATER 0)
  # A compiler-produced large constant fills the synthetic 64 KiB store.
  string(REPEAT x 24576 padding)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/padded.py "padding = b'${padding}'\nprint('padded')\n")
  list(APPEND programs padded)
endif()
foreach(program ${programs})
  set(source ${CMAKE_CURRENT_SOURCE_DIR}/${program}.py)
  if(program STREQUAL padded)
    set(source ${CMAKE_CURRENT_BINARY_DIR}/padded.py)
  elseif(program STREQUAL native)
    set(source ${BRICKPHYR_ROOT}/tests/python/native.py)
  endif()
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${program}.mpy
    COMMAND ${CMAKE_BINARY_DIR}/mpy-cross/mpy-cross -march=armv7m -s ${program}.py
      -o ${CMAKE_CURRENT_BINARY_DIR}/${program}.mpy ${source}
    DEPENDS mpy_cross ${source} VERBATIM)
  generate_inc_file_for_target(app ${CMAKE_CURRENT_BINARY_DIR}/${program}.mpy
    ${ZEPHYR_BINARY_DIR}/include/generated/${program}.inc)
endforeach()
