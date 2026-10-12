// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	const uint8_t level = 100;
	int invalid = bp_matrix_write(UINT32_MAX, &level, 1);
	if (invalid != -EINVAL) {
		return 1000 + invalid;
	}
	invalid = bp_matrix_write(0, &level, SIZE_MAX);
	if (invalid != -EINVAL) {
		return 2000 + invalid;
	}
	uint8_t frames[2 * BP_MATRIX_PIXELS] = {100};
	int err = bp_matrix_animate(frames, 2, 10);
	if (err) {
		return err;
	}
	if (bp_buttons()) {
		return bp_matrix_animate((const void *)(UINTPTR_MAX - 3), 2, 10);
	}
	/* A prohibited result buffer must end only this application and release its
	 * display, even though its previously accepted write may still be running. */
	return bp_matrix_get((struct bp_matrix_state *)0xfffffff0U);
}
