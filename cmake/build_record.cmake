# SPDX-License-Identifier: Apache-2.0
include_guard(GLOBAL)

set(source_state ${CMAKE_BINARY_DIR}/brickphyr-source.json)
set(build_record ${ZEPHYR_BINARY_DIR}/brickphyr-build.json)
# Run on each build, preserving the file's mtime when nothing changed. A changed
# checkout must relink before it can acquire new source metadata.
add_custom_target(brickphyr-source-state
  COMMAND ${PYTHON_EXECUTABLE} -m tools.build_record snapshot ${CMAKE_BINARY_DIR}
    "--version=${APP_VERSION_STRING}"
  BYPRODUCTS ${source_state}
  WORKING_DIRECTORY ${BRICKPHYR_ROOT}
  VERBATIM)
add_dependencies(zephyr_final brickphyr-source-state)
set_property(TARGET zephyr_final APPEND PROPERTY LINK_DEPENDS ${source_state})
if(TARGET app_version_h)
  set_property(TARGET app_version_h APPEND PROPERTY APP_VERSION_DEPENDS ${source_state})
endif()

set(image_files $<TARGET_FILE:zephyr_final>)
if(CONFIG_BUILD_OUTPUT_BIN)
  list(APPEND image_files ${ZEPHYR_BINARY_DIR}/zephyr.bin)
endif()
add_custom_command(OUTPUT ${build_record}
  COMMAND ${PYTHON_EXECUTABLE} -m tools.build_record finish ${CMAKE_BINARY_DIR}
  DEPENDS ${image_files} ${source_state} ${ZEPHYR_BINARY_DIR}/.config
    ${ZEPHYR_BINARY_DIR}/zephyr.dts ${BRICKPHYR_ROOT}/tools/build_record.py
  WORKING_DIRECTORY ${BRICKPHYR_ROOT}
  VERBATIM)
add_custom_target(brickphyr-build-record ALL DEPENDS ${build_record})
