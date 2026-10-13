# SPDX-License-Identifier: Apache-2.0
set(python_native_arch armv7m)
if(CONFIG_FP_HARDABI AND CONFIG_CPU_CORTEX_M4)
  set(python_native_arch armv7emsp)
endif()
