# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(host_radio)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/host_ble.c
  ${BRICKPHYR_ROOT}/src/host_protocol.c
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/src/cobs.c
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/src/protocol/pybricks.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
zephyr_ld_options(-Wl,--wrap=bt_enable -Wl,--wrap=bt_disable
  -Wl,--wrap=bt_le_adv_start -Wl,--wrap=bt_le_adv_stop
  -Wl,--wrap=bt_conn_ref -Wl,--wrap=bt_conn_unref -Wl,--wrap=bt_conn_disconnect
  -Wl,--wrap=bt_gatt_is_subscribed -Wl,--wrap=bt_gatt_notify_cb -Wl,--wrap=bt_gatt_get_mtu)
