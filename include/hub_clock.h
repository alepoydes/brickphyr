// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <zephyr/kernel.h>

#define HUB_TIME_RESOLUTION_US DIV_ROUND_UP(1000000, \
	IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER) ? \
	CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC : CONFIG_SYS_CLOCK_TICKS_PER_SEC)

static inline int64_t hub_time_us(void)
{
	if (IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER)) {
		return k_cyc_to_us_floor64(k_cycle_get_64());
	}
	return k_ticks_to_us_floor64(k_uptime_ticks());
}
