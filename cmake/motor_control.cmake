# SPDX-License-Identifier: Apache-2.0
set(pbio ${WEST_TOPDIR}/pybricks-micropython/lib/pbio)
target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/motor/control.c
  ${pbio}/src/angle.c ${pbio}/src/int_math.c ${pbio}/src/trajectory.c
  ${pbio}/src/integrator.c ${pbio}/src/control_settings.c ${pbio}/src/control.c
  ${pbio}/src/differentiator.c ${pbio}/src/observer.c ${pbio}/src/util.c)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/src/motor
  ${BRICKPHYR_ROOT}/sdk/include ${BRICKPHYR_ROOT}/drivers/pybricks ${pbio}/include ${pbio}/../lego)
