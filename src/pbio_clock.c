// SPDX-License-Identifier: Apache-2.0
#include <zephyr/kernel.h>
#include <pbdrv/clock.h>

/* The same user-accessible monotonic clock serves PBIO tools and the native
 * controller logger. Neither depends on a running Python application. */
uint32_t pbdrv_clock_get_ms(void)
{
	return k_uptime_get_32();
}
