// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	const uint8_t red[] = {255, 0, 0};
	int err = bp_light_write(BP_LIGHT_CENTER, red);
	if (err) {
		return err;
	}
	if (bp_buttons() & 1) {
		struct bp_light_state state;
		return bp_light_get(BP_LIGHT_CENTER, (void *)((uint8_t *)&state + 1));
	}
	return bp_light_write(BP_LIGHT_CENTER, (const uint8_t *)UINTPTR_MAX);
}
