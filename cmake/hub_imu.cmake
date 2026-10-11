# SPDX-License-Identifier: Apache-2.0
set(imu_registers ${WEST_TOPDIR}/pybricks-micropython/lib/lsm6ds3tr_c_STdC/driver)
target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/hub_imu.c ${BRICKPHYR_ROOT}/src/hub_imu_state.c
  ${BRICKPHYR_ROOT}/src/imu/processing.c ${BRICKPHYR_ROOT}/src/imu/stationary.c
  ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/src/geometry.c
  ${imu_registers}/lsm6ds3tr_c_reg.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include
  ${BRICKPHYR_ROOT}/sdk/include ${BRICKPHYR_ROOT}/drivers/pybricks
  ${imu_registers} ${WEST_TOPDIR}/pybricks-micropython/lib/pbio/include)
