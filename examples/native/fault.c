// SPDX-License-Identifier: Apache-2.0
/* Diagnostic: writes to the read-only firmware API must fault, regardless of
 * where a board places the application's RAM and stack. */
#include <brickphyr.h>
BRICKPHYR_APPLICATION();

int app_main(void)
{
	*(volatile uint32_t *)(uintptr_t)&bp_api_v1 = 0;
	return 1;
}
