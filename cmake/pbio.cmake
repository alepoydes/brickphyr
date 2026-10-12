# SPDX-License-Identifier: Apache-2.0
include_guard(GLOBAL)

# Portable helpers shared by drivers, host protocols and Python bindings.
set(pbio ${WEST_TOPDIR}/pybricks-micropython/lib/pbio)
target_sources(app PRIVATE ${pbio}/src/int_math.c ${pbio}/src/util.c
  ${pbio}/src/error.c ${pbio}/src/cobs.c ${pbio}/src/color/conversion.c
  ${pbio}/src/geometry.c)
target_include_directories(app PRIVATE ${pbio}/include)
