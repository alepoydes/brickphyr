// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	/* This example moves port A. Use an unloaded, freely rotating motor. */
	const struct bp_motor_command command = {
		.mode = BP_MOTOR_SPEED,
		.speed_mdeg_s = 200000,
		.duration_ms = 250,
		.finish = BP_MOTOR_FINISH_COAST,
	};
	int err = bp_motor_control(0, &command);
	if (err) return err;
	for (unsigned waited = 0; waited < 1000; waited += 10) {
		struct bp_motor_state state;
		err = bp_motor_get(0, &state);
		if (err || state.error) return err ? err : state.error;
		if (!state.pending && state.control.done) return 0;
		err = bp_sleep(10);
		if (err) return err;
	}
	/* Application cleanup coasts and releases the port on every exit. */
	return -ETIMEDOUT;
}
