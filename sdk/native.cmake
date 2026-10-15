# SPDX-License-Identifier: Apache-2.0
# Included after Zephyr configuration by firmware and ARM acceptance fixtures.
include(${BRICKPHYR_ROOT}/cmake/runtime_memory.cmake)
zephyr_include_directories(${BRICKPHYR_ROOT}/sdk/include)
zephyr_syscall_header(${BRICKPHYR_ROOT}/sdk/include/brickphyr.h)
zephyr_syscall_header(${BRICKPHYR_ROOT}/include/application_private.h)
set(native_api ${ZEPHYR_BINARY_DIR}/include/generated/brickphyr_api.h)
add_custom_command(OUTPUT ${native_api}
    ${ZEPHYR_BINARY_DIR}/include/generated/brickphyr_api.inc
  COMMAND ${PYTHON_EXECUTABLE} ${BRICKPHYR_ROOT}/tools/native_api.py
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr.h ${ZEPHYR_BINARY_DIR}/include/generated
  DEPENDS ${BRICKPHYR_ROOT}/sdk/include/brickphyr.h
    ${BRICKPHYR_ROOT}/tools/native_api.py
  VERBATIM)
# Retire the earlier configuration hash from reused build directories.
file(REMOVE ${ZEPHYR_BINARY_DIR}/include/generated/brickphyr_abi.h)
add_custom_target(brickphyr_api DEPENDS ${native_api})
add_dependencies(app brickphyr_api)
target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/application.c ${BRICKPHYR_ROOT}/src/native_image.c ${BRICKPHYR_ROOT}/src/native_api.c
  ${BRICKPHYR_ROOT}/src/diagnostics.c ${BRICKPHYR_ROOT}/src/control_frame.c
  ${BRICKPHYR_ROOT}/src/application_record.c)
# The resident table is an application entry point, so retain only SVC wrappers.
set_source_files_properties(${BRICKPHYR_ROOT}/src/native_api.c PROPERTIES
  COMPILE_DEFINITIONS __ZEPHYR_USER__)
target_include_directories(app PRIVATE ${BRICKPHYR_ROOT}/include)
if(CONFIG_BRICKPHYR_BLE_PEER)
  target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/hub_ble_peer.c ${BRICKPHYR_ROOT}/src/lwp3_devices.c
    ${BRICKPHYR_ROOT}/src/xbox_device.c)
  target_include_directories(app PRIVATE ${WEST_TOPDIR}/pybricks-micropython/lib/lego)
  include(${BRICKPHYR_ROOT}/cmake/pbio.cmake)
endif()
if(CONFIG_BRICKPHYR_HUB_NETWORK)
  target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/hub_network.c)
endif()
if(CONFIG_MCUMGR)
  target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/application_mgmt.c)
endif()

if(CONFIG_BRICKPHYR_NATIVE_FIXTURES)
  set(native_examples hello counter spin fault matrix matrix_fault light_fault imu sound sound_fault battery_fault system_fault unsupported motor)
  if(CONFIG_FPU)
    list(APPEND native_examples fpu)
  endif()
  if(CONFIG_CMSIS_DSP_FILTERING)
    list(APPEND native_examples dsp)
  endif()
  if(NATIVE_CAPACITY_TEST)
    list(APPEND native_examples capacity)
    target_compile_definitions(app PRIVATE NATIVE_CAPACITY_TEST)
  endif()
  foreach(example ${native_examples})
    # Run an independently built SDK image unchanged in different firmware
    # configurations; ordinary fixtures still compile their local examples.
    if(example STREQUAL "hello" AND NATIVE_HELLO_IMAGE)
      generate_inc_file_for_target(app ${NATIVE_HELLO_IMAGE}
        ${ZEPHYR_BINARY_DIR}/include/generated/native_hello.inc)
      continue()
    endif()
    if(example MATCHES "_fault$" OR example MATCHES "^(motor|fpu|dsp|unsupported|capacity)$")
      set(example_source ${BRICKPHYR_ROOT}/tests/native/${example}.c)
    else()
      set(example_source ${BRICKPHYR_ROOT}/examples/native/${example}.c)
    endif()
    add_llext_target(native_${example}
      OUTPUT ${PROJECT_BINARY_DIR}/${example}.elf
      SOURCES ${example_source})
    add_dependencies(native_${example}_llext_lib brickphyr_api)
    llext_compile_options(native_${example} -O2 -g0)
    llext_link_options(native_${example} -nostdlib
      -Wl,-T,${BRICKPHYR_ROOT}/sdk/include/brickphyr.ld.h)
    get_target_property(example_target native_${example} lib_target)
    if(example STREQUAL "dsp")
      # Use the archive without inheriting Zephyr's firmware link interface.
      target_link_libraries(${example_target} PRIVATE $<TARGET_FILE:modules__cmsis-dsp>)
      add_dependencies(${example_target} modules__cmsis-dsp)
    endif()
    target_link_libraries(${example_target} PRIVATE c gcc)
    set_property(TARGET native_${example}_llext_lib APPEND PROPERTY LINK_DEPENDS
      ${BRICKPHYR_ROOT}/sdk/include/brickphyr.ld.h)
    generate_inc_file_for_target(app ${PROJECT_BINARY_DIR}/${example}.elf
      ${ZEPHYR_BINARY_DIR}/include/generated/native_${example}.inc)
    add_dependencies(app native_${example})
  endforeach()
  target_sources(app PRIVATE ${BRICKPHYR_ROOT}/tests/native/fixtures.c)
endif()

if(CONFIG_BRICKPHYR_APP_STORAGE)
  target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/app_store.c ${BRICKPHYR_ROOT}/src/app_data.c)
endif()

if(CONFIG_BRICKPHYR_PYTHON)
  add_subdirectory(${BRICKPHYR_ROOT}/src/python ${CMAKE_BINARY_DIR}/python)
endif()

if(CONFIG_LLEXT_EDK)
  set(sdk_python_args)
  if(CONFIG_BRICKPHYR_PYTHON)
    list(APPEND sdk_python_args --micropython ${WEST_TOPDIR}/pybricks-micropython/micropython)
  endif()
  set(sdk_dsp_args)
  if(CONFIG_CMSIS_DSP)
    list(APPEND sdk_dsp_args --dsp $<TARGET_FILE:modules__cmsis-dsp>
      ${ZEPHYR_CMSIS_DSP_MODULE_DIR}/LICENSE)
  endif()
  add_custom_target(brickphyr-sdk
    COMMAND ${PYTHON_EXECUTABLE} ${BRICKPHYR_ROOT}/tools/export_sdk.py
      ${CMAKE_BINARY_DIR} ${sdk_python_args} ${sdk_dsp_args}
    DEPENDS llext-edk brickphyr_api
    VERBATIM)
  if(CONFIG_BRICKPHYR_PYTHON)
    add_dependencies(brickphyr-sdk mpy_cross)
  endif()
  if(CONFIG_CMSIS_DSP)
    add_dependencies(brickphyr-sdk modules__cmsis-dsp)
  endif()
endif()
