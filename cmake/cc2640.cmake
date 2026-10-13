# SPDX-License-Identifier: Apache-2.0
include_guard(GLOBAL)
# Reuse explicitly BSD-licensed TI protocol headers. The unmarked hci.c and
# hci_ext.c implementations have no clear grant under Pybricks' root policy.
set(ti_central ${WEST_TOPDIR}/pybricks-micropython/lib/ble5stack/central)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../drivers/cc2640_controller.c
  ${CMAKE_CURRENT_LIST_DIR}/../drivers/cc2640_gatt.c
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/src/protocol/pybricks.c)
target_include_directories(app PRIVATE ${ti_central}
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
