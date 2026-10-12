# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.5)
get_filename_component(BRICKPHYR_ROOT ../.. ABSOLUTE)
set(KCONFIG_ROOT ${BRICKPHYR_ROOT}/Kconfig)
list(APPEND DTS_ROOT ${BRICKPHYR_ROOT} ${BRICKPHYR_ROOT}/tests/watchdog)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hub_poweroff)
target_sources(app PRIVATE main.c ${BRICKPHYR_ROOT}/src/hub.c
  ../watchdog/device.c ${BRICKPHYR_ROOT}/src/hub_watchdog.c ${BRICKPHYR_ROOT}/src/hub_reset.c
  ${BRICKPHYR_ROOT}/src/hub_menu.c ${BRICKPHYR_ROOT}/src/hub_power.c ${BRICKPHYR_ROOT}/src/configuration.c
  ${BRICKPHYR_ROOT}/drivers/input_resistor_ladder.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include ${BRICKPHYR_ROOT}/sdk/include
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
zephyr_ld_options(-Wl,--wrap=sys_reboot)
