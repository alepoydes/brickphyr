// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	/* Keep the display owned until the operator stops the application. */
	const uint8_t smile[BP_MATRIX_PIXELS] = {
		0, 100, 0, 100, 0,
		0, 100, 0, 100, 0,
		0,   0, 0,   0, 0,
		100, 0, 0, 0, 100,
		0, 100, 100, 100, 0,
	};
	int err = bp_matrix_write(0, smile, sizeof(smile));
	if (err) {
		return err;
	}
	for (;;) {
		struct bp_matrix_state state;
		err = bp_matrix_get(&state);
		if (err || state.error) {
			return err ? err : state.error;
		}
		bp_sleep(100);
	}
}
