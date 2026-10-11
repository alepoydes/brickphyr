// SPDX-License-Identifier: Apache-2.0
/* Diagnostic: the MPU must reject writes to privileged firmware RAM. */
#include <brickphyr.h>
BRICKPHYR_APPLICATION();

int app_main(void)
{
	*(volatile uint32_t *)0x20000000 = 0;
	return 1;
}
