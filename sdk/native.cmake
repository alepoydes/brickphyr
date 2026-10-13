# SPDX-License-Identifier: Apache-2.0
# Included after Zephyr configuration by firmware and ARM acceptance fixtures.
zephyr_linker_sources(RAM_SECTIONS ${BRICKPHYR_ROOT}/sdk/execution.ld)
zephyr_include_directories(${BRICKPHYR_ROOT}/sdk/include)
zephyr_syscall_header(${BRICKPHYR_ROOT}/sdk/include/brickphyr.h)
zephyr_syscall_header(${BRICKPHYR_ROOT}/include/application_private.h)
set(native_abi ${ZEPHYR_BINARY_DIR}/include/generated/brickphyr_abi.h)
add_custom_command(OUTPUT ${native_abi}
  COMMAND ${PYTHON_EXECUTABLE} ${BRICKPHYR_ROOT}/tools/native_abi.py ${native_abi}
    ${ZEPHYR_BINARY_DIR}/include/generated/zephyr/autoconf.h
    ${ZEPHYR_BINARY_DIR}/include/generated/zephyr/syscall_list.h
    ${ZEPHYR_BASE}/VERSION ${BRICKPHYR_ROOT}/sdk/include/brickphyr.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_matrix.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_light.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_radio.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_lwp3.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_ble_peer.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_network.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_xbox.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_imu.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_sound.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_battery.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_charger.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_port.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_sensor.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_motor.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_drivebase.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_system.h
    ${ZEPHYR_BASE}/include/zephyr/arch/arm/syscall.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr.ld.h
  DEPENDS ${ZEPHYR_BINARY_DIR}/include/generated/zephyr/autoconf.h
    ${ZEPHYR_BINARY_DIR}/include/generated/zephyr/syscall_list.h
    ${ZEPHYR_BASE}/VERSION ${BRICKPHYR_ROOT}/sdk/include/brickphyr.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_matrix.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_light.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_radio.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_lwp3.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_ble_peer.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_network.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_xbox.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_imu.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_sound.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_battery.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_charger.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_port.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_sensor.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_motor.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_drivebase.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr_system.h
    ${ZEPHYR_BASE}/include/zephyr/arch/arm/syscall.h
    ${BRICKPHYR_ROOT}/sdk/include/brickphyr.ld.h
    ${BRICKPHYR_ROOT}/tools/native_abi.py
  VERBATIM)
add_custom_target(brickphyr_abi DEPENDS ${native_abi})
add_dependencies(app brickphyr_abi)
target_sources(app PRIVATE ${BRICKPHYR_ROOT}/src/application.c ${BRICKPHYR_ROOT}/src/native_image.c
  ${BRICKPHYR_ROOT}/src/diagnostics.c)
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
  set(native_examples hello counter spin fault matrix matrix_fault light_fault imu sound sound_fault battery_fault system_fault data_fault motor)
  if(CONFIG_FPU)
    list(APPEND native_examples fpu)
  endif()
  if(CONFIG_CMSIS_DSP_FILTERING)
    list(APPEND native_examples dsp)
  endif()
  foreach(example ${native_examples})
    if(example MATCHES "_fault$" OR example MATCHES "^(motor|fpu|dsp)$")
      set(example_source ${BRICKPHYR_ROOT}/tests/native/${example}.c)
    else()
      set(example_source ${BRICKPHYR_ROOT}/examples/native/${example}.c)
    endif()
    add_llext_target(native_${example}
      OUTPUT ${PROJECT_BINARY_DIR}/${example}.elf
      SOURCES ${example_source})
    add_dependencies(native_${example}_llext_lib brickphyr_abi)
    # Firmware headers include both syscall paths; fixture images use only SVC.
    llext_compile_definitions(native_${example} __ZEPHYR_USER__)
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
    DEPENDS llext-edk
    VERBATIM)
  if(CONFIG_BRICKPHYR_PYTHON)
    add_dependencies(brickphyr-sdk mpy_cross)
  endif()
  if(CONFIG_CMSIS_DSP)
    add_dependencies(brickphyr-sdk modules__cmsis-dsp)
  endif()
endif()
