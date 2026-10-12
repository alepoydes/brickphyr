# SPDX-License-Identifier: Apache-2.0
# Build the compiler from the same checkout as the runtime, out of source.
find_program(MICROPY_MAKE NAMES gmake make REQUIRED)
set(MICROPY_CROSS_BUILD ${CMAKE_BINARY_DIR}/mpy-cross)
set(MICROPY_CROSS ${MICROPY_CROSS_BUILD}/mpy-cross)
add_custom_target(mpy_cross
  COMMAND ${MICROPY_MAKE} -C ${MICROPY_DIR}/mpy-cross
    BUILD=${MICROPY_CROSS_BUILD} PYTHON=${Python3_EXECUTABLE}
  BYPRODUCTS ${MICROPY_CROSS}
  VERBATIM USES_TERMINAL)
