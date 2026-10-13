# SPDX-License-Identifier: Apache-2.0
# Build independent .mpy producers with upstream's compiler and dynruntime rules.
set(python_fixtures ${CMAKE_BINARY_DIR}/python-fixtures)
file(MAKE_DIRECTORY ${python_fixtures})
set(micropython ${WEST_TOPDIR}/pybricks-micropython/micropython)
string(REGEX REPLACE "gcc$" "" compiler_prefix ${CMAKE_C_COMPILER})
include(${BRICKPHYR_ROOT}/cmake/python_arch.cmake)
file(GLOB runtime_headers CONFIGURE_DEPENDS ${micropython}/py/*.h)
# Reconfigure-generated dependency also rebuilds the fixed output paths when
# switching back to an architecture whose objects are already in build-${ARCH}.
file(GENERATE OUTPUT ${python_fixtures}/native-arch.txt CONTENT "${python_native_arch}\n")

add_custom_command(OUTPUT ${python_fixtures}/native.mpy
  COMMAND ${CMAKE_BINARY_DIR}/mpy-cross/mpy-cross -march=${python_native_arch} -s native.py
    -o ${python_fixtures}/native.mpy ${BRICKPHYR_ROOT}/tests/python/native.py
  DEPENDS mpy_cross ${BRICKPHYR_ROOT}/tests/python/native.py
    ${python_fixtures}/native-arch.txt VERBATIM)
set(python_images ${python_fixtures}/native.mpy)
foreach(module native_module native_code_fault native_heap_fault)
  add_custom_command(OUTPUT ${python_fixtures}/${module}.mpy
    # Upstream keeps objects in build-${ARCH}, but its final .mpy has a shared name.
    COMMAND ${CMAKE_COMMAND} -E rm -f ${python_fixtures}/${module}.mpy
    COMMAND ${MICROPY_MAKE} -f ${BRICKPHYR_ROOT}/tests/python/Makefile
      MPY_DIR=${micropython} MOD=${module} SRC=${module}.c
      VPATH=${BRICKPHYR_ROOT}/tests/python ARCH=${python_native_arch} CROSS=${compiler_prefix}
      PYTHON=${PYTHON_EXECUTABLE}
    WORKING_DIRECTORY ${python_fixtures}
    DEPENDS ${BRICKPHYR_ROOT}/tests/python/${module}.c
      ${BRICKPHYR_ROOT}/tests/python/Makefile
      ${micropython}/py/dynruntime.mk
      ${micropython}/tools/mpy_ld.py ${runtime_headers}
      ${python_fixtures}/native-arch.txt VERBATIM)
  list(APPEND python_images ${python_fixtures}/${module}.mpy)
endforeach()
add_custom_target(python_native_fixtures ALL DEPENDS ${python_images})
