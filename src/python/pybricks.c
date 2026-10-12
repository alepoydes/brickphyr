// SPDX-License-Identifier: Apache-2.0
#include "py/mphal.h"
#include <pbdrv/clock.h>

/* Pybricks tools use the same user-accessible monotonic clock as the VM. */
uint32_t pbdrv_clock_get_ms(void)
{
	return mp_hal_ticks_ms();
}
