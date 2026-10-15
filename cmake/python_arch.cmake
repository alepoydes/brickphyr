# SPDX-License-Identifier: Apache-2.0
if(CONFIG_CPU_CORTEX_M AND NOT (CONFIG_CPU_CORTEX_M4 AND CONFIG_FP_HARDABI))
  message(FATAL_ERROR "Brickphyr ARM Python requires Cortex-M4 with hard-float ABI")
endif()
# The host compiler also produces this target when built by native_sim tests.
set(python_native_arch armv7emsp)
