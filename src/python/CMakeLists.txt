# SPDX-License-Identifier: Apache-2.0
set(MICROPY_DIR ${WEST_TOPDIR}/pybricks-micropython/micropython)
set(MICROPY_TARGET brickphyr_python)
include(${MICROPY_DIR}/py/py.cmake)

set(MICROPY_SOURCE_QSTR ${MICROPY_SOURCE_PY}
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime.c ${CMAKE_CURRENT_SOURCE_DIR}/files.c
  ${CMAKE_CURRENT_SOURCE_DIR}/streams.c
  ${MICROPY_DIR}/shared/readline/readline.c
  ${MICROPY_DIR}/shared/runtime/pyexec.c
  ${MICROPY_DIR}/extmod/modjson.c ${MICROPY_DIR}/extmod/modrandom.c
  ${MICROPY_DIR}/extmod/modselect.c)
if(CONFIG_BRICKPHYR_PYBRICKS)
  set(pybricks ${WEST_TOPDIR}/pybricks-micropython)
  set(MICROPY_QSTRDEFS_PORT ${pybricks}/bricks/_common/qstrdefs.h)
  list(APPEND MICROPY_SOURCE_QSTR
    ${pybricks}/pybricks/pybricks.c
    ${pybricks}/pybricks/parameters/pb_module_parameters.c
    ${pybricks}/pybricks/parameters/pb_type_axis.c
    ${pybricks}/pybricks/parameters/pb_type_button.c
    ${pybricks}/pybricks/parameters/pb_type_color.c
    ${pybricks}/pybricks/parameters/pb_type_direction.c
    ${pybricks}/pybricks/parameters/pb_type_icon.c
    ${pybricks}/pybricks/parameters/pb_type_port.c
    ${pybricks}/pybricks/parameters/pb_type_side.c
    ${pybricks}/pybricks/parameters/pb_type_stop.c
    ${pybricks}/pybricks/tools/pb_module_tools.c
    ${pybricks}/pybricks/tools/pb_type_async.c
    ${pybricks}/pybricks/tools/pb_type_matrix.c
    ${pybricks}/pybricks/tools/pb_type_stopwatch.c
    ${pybricks}/pybricks/tools/pb_type_task.c
    ${pybricks}/pybricks/util_mp/pb_obj_helper.c
    ${pybricks}/pybricks/util_mp/pb_type_enum.c
    ${pybricks}/pybricks/util_pb/pb_error.c)
  if(CONFIG_BRICKPHYR_NATIVE)
    list(APPEND MICROPY_SOURCE_QSTR
      ${CMAKE_CURRENT_SOURCE_DIR}/hub.c ${CMAKE_CURRENT_SOURCE_DIR}/light.c
      ${CMAKE_CURRENT_SOURCE_DIR}/matrix.c ${CMAKE_CURRENT_SOURCE_DIR}/speaker.c
      ${CMAKE_CURRENT_SOURCE_DIR}/system.c ${CMAKE_CURRENT_SOURCE_DIR}/imu.c
      ${CMAKE_CURRENT_SOURCE_DIR}/pupdevice.c ${CMAKE_CURRENT_SOURCE_DIR}/pupsensors.c
      ${CMAKE_CURRENT_SOURCE_DIR}/pupaccessories.c
      ${pybricks}/pybricks/hubs/pb_module_hubs.c
      ${pybricks}/pybricks/common/pb_type_keypad.c
      ${pybricks}/pybricks/common/pb_type_colorlight_external.c
      ${pybricks}/pybricks/common/pb_type_lightmatrix_fonts.c)
  endif()
  include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/pbio.cmake)
endif()
set(gc_sources ${MICROPY_DIR}/shared/runtime/gchelper_generic.c)
if(CONFIG_CPU_CORTEX_M)
  set(gc_sources ${MICROPY_DIR}/shared/runtime/gchelper_native.c
    ${MICROPY_DIR}/shared/runtime/gchelper_thumb2.s)
endif()

zephyr_library_named(${MICROPY_TARGET})
zephyr_library_sources(${MICROPY_SOURCE_QSTR} ${gc_sources}
  ${MICROPY_DIR}/shared/runtime/stdout_helpers.c)
zephyr_library_include_directories(${MICROPY_INC_CORE} ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/include)
zephyr_library_compile_definitions(NDEBUG __ZEPHYR_USER__)
zephyr_library_compile_options(-std=gnu99 -fomit-frame-pointer)
if(CONFIG_BRICKPHYR_PYBRICKS)
  zephyr_library_sources(pybricks.c)
  zephyr_library_include_directories(${pybricks} ${pybricks}/lib/pbio/include
    ${pybricks}/lib/lego ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/pybricks)
endif()
target_link_libraries(app PRIVATE ${MICROPY_TARGET})
if(CONFIG_BRICKPHYR_NATIVE)
  zephyr_library_sources(io.c native.c package.c)
  add_dependencies(${MICROPY_TARGET} brickphyr_abi)
endif()
if(CONFIG_USERSPACE)
  if(CONFIG_NEED_LIBC_MEM_PARTITION)
    # One runtime-data region leaves MPU space for separate native code and GC.
    zephyr_library_app_memory(z_libc_partition)
  else()
    zephyr_library_app_memory(python_partition)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/memory.c)
  endif()
endif()

zephyr_get_include_directories_for_lang(C includes)
zephyr_get_system_include_directories_for_lang(C system_includes)
zephyr_get_compile_definitions_for_lang(C definitions)
zephyr_get_compile_options_for_lang(C options)
set(MICROPY_CPP_FLAGS_EXTRA ${includes} ${system_includes} ${definitions} ${options})
include(${MICROPY_DIR}/py/mkrules.cmake)
add_custom_command(OUTPUT ${MICROPY_QSTRDEFS_LAST} APPEND
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mpconfigport.h
    ${ZEPHYR_BINARY_DIR}/include/generated/zephyr/autoconf.h)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/mpy_cross.cmake)
add_dependencies(BUILD_VERSION_HEADER zephyr_generated_headers)
add_dependencies(${MICROPY_TARGET} zephyr_generated_headers)
