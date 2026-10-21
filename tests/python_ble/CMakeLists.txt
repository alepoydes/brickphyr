# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
set(CONF_FILE ../arm.conf ../native/prj.conf prj.conf)
list(APPEND SYSCALL_INCLUDE_DIRS ${BRICKPHYR_ROOT}/sdk/include ${BRICKPHYR_ROOT}/include)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(python_ble)
set(FOCUSED_TEST "" CACHE STRING "Run one named python_ble regression")
if(FOCUSED_TEST STREQUAL "test_hub_network_classic_routing_and_lifetime")
  target_compile_definitions(app PRIVATE BRICKPHYR_FOCUSED_NETWORK_TEST=1)
elseif(FOCUSED_TEST STREQUAL "test_native_terminal_failed_notify_retires_link")
  target_compile_definitions(app PRIVATE BRICKPHYR_FOCUSED_TERMINAL_FAILURE_TEST=1)
elseif(FOCUSED_TEST STREQUAL "test_hidden_installed_programs")
  target_compile_definitions(app PRIVATE BRICKPHYR_FOCUSED_STORE_TEST=1)
elseif(FOCUSED_TEST STREQUAL "test_ble_peer_retry_and_unnamed")
  target_compile_definitions(app PRIVATE BRICKPHYR_FOCUSED_PEER_TEST=1)
elseif(FOCUSED_TEST STREQUAL "test_shared_disconnect_recoverable")
  target_compile_definitions(app PRIVATE BRICKPHYR_FOCUSED_DISCONNECT_RECOVERABLE_TEST=1)
elseif(FOCUSED_TEST STREQUAL "test_shared_disconnect_missing_callback")
  target_compile_definitions(app PRIVATE BRICKPHYR_FOCUSED_DISCONNECT_TIMEOUT_TEST=1)
elseif(FOCUSED_TEST)
  message(FATAL_ERROR "Unknown FOCUSED_TEST: ${FOCUSED_TEST}")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/pbio.cmake)
include(${BRICKPHYR_ROOT}/sdk/native.cmake)
target_sources_ifdef(CONFIG_BRICKPHYR_APP_STORAGE app PRIVATE ${BRICKPHYR_ROOT}/src/storage_mount.c)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/host_transport.c
	${BRICKPHYR_ROOT}/drivers/ti_cc2564c.c
  ${BRICKPHYR_ROOT}/src/host_protocol.c ${BRICKPHYR_ROOT}/src/host_ble.c
  ${BRICKPHYR_ROOT}/src/host_ble_session.c
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
  -Wl,--wrap=bt_conn_get_security
  -Wl,--wrap=bt_conn_get_dst -Wl,--wrap=bt_unpair
  -Wl,--wrap=bt_br_oob_get_local -Wl,--wrap=bt_br_set_connectable
  -Wl,--wrap=bt_l2cap_br_server_register -Wl,--wrap=bt_conn_create_br
  -Wl,--wrap=bt_conn_lookup_addr_br -Wl,--wrap=bt_l2cap_chan_connect
  -Wl,--wrap=bt_l2cap_chan_send
  -Wl,--wrap=bt_gatt_discover -Wl,--wrap=bt_gatt_subscribe -Wl,--wrap=bt_gatt_write
  -Wl,--wrap=bt_gatt_read
  -Wl,--wrap=bt_gatt_is_subscribed -Wl,--wrap=bt_gatt_notify_cb
  -Wl,--wrap=bt_gatt_get_mtu -Wl,--wrap=bt_gatt_get_uatt_mtu)
include(${BRICKPHYR_ROOT}/cmake/python_arch.cmake)
set(programs echo spin flood native messaging radio lwp3 wireless xbox network)
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
    COMMAND ${CMAKE_BINARY_DIR}/mpy-cross/mpy-cross -march=${python_native_arch} -s ${program}.py
      -o ${CMAKE_CURRENT_BINARY_DIR}/${program}.mpy ${source}
    DEPENDS mpy_cross ${source} VERBATIM)
  generate_inc_file_for_target(app ${CMAKE_CURRENT_BINARY_DIR}/${program}.mpy
    ${ZEPHYR_BINARY_DIR}/include/generated/${program}.inc)
endforeach()
