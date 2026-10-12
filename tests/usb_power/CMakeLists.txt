# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(usb_power)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/host_usb_power.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include
  ${BRICKPHYR_ROOT}/sdk/include)
zephyr_ld_options(-Wl,--wrap=usbd_enable -Wl,--wrap=usbd_disable)
# Retain the normal USB context registry without linking an emulated bus stack.
zephyr_linker_sources(DATA_SECTIONS ${ZEPHYR_BASE}/subsys/usb/device_next/usbd_data.ld)
