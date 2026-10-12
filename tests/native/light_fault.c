// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	const struct bp_light_frame frames[] = {{10, {255, 0, 0}}, {10, {0, 0, 255}}};
	int err = bp_light_animate(BP_LIGHT_CENTER, frames, 2);
	if (err) {
		return err;
	}
	if (bp_buttons() == 1) {
		struct bp_light_state state;
		return bp_light_get(BP_LIGHT_CENTER, (void *)((uint8_t *)&state + 1));
	}
	if (bp_buttons() == 2) {
		return bp_light_animate(BP_LIGHT_CENTER, (const void *)(UINTPTR_MAX - 3), 1);
	}
	if (bp_buttons() == 3) {
		return bp_light_animate(BP_LIGHT_CENTER, (const void *)((const uint8_t *)frames + 1), 1);
	}
	return bp_light_write(BP_LIGHT_CENTER, (const uint8_t *)UINTPTR_MAX);
}
