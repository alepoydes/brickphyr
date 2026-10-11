// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	int err = bp_motor_drive(0, 250, 0);
	if (err) return err;
	struct bp_motor_state state;
	int64_t deadline = bp_time_ms() + 200;
	do {
		err = bp_motor_get(0, &state);
		if (err || state.error) return err ? err : state.error;
		if (bp_time_ms() >= deadline) return -ETIMEDOUT;
		bp_sleep(1);
	} while (state.pending);
	if (!state.valid || state.duty != 250 || state.owner != BP_PORT_APPLICATION) return -EINVAL;
	/* Synthetic fixture selector: normal return, supervisor stop, or an invalid
	 * syscall buffer while driving. None may leave PWM running after cleanup. */
	uint32_t behavior = bp_buttons();
	if (behavior == 2) {
		uint64_t storage[sizeof(state) / sizeof(uint64_t) + 1];
		return bp_motor_get(0, (struct bp_motor_state *)((uint8_t *)storage + 1));
	}
	if (behavior == 1) for (;;) bp_sleep(10);
	return 0;
}
