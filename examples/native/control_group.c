// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_PORTS | BP_CAP_PORT_UART | BP_CAP_MOTORS, 2048);

static struct bp_control_frame inputs;

int app_main(void)
{
	/* This example moves A/B encoder motors. Clear both mechanisms. */
	const uint32_t mask = (1U << 0) | (1U << 1);
	int err = bp_control_frame_get(mask, &inputs);
	if (err) return err;
	struct bp_motor_group_command command = {
		.port_mask = mask, .deadline_ms = bp_time_ms() + 100,
	};
	for (uint32_t port = 0; port < 2; port++) {
		if (inputs.ports[port].error) return inputs.ports[port].error;
		command.ports[port].generation = inputs.ports[port].generation;
		command.ports[port].duty_permille = port ? -200 : 200;
	}
	uint32_t request;
	err = bp_motor_group_set(&command, &request);
	if (err) return err;
	for (;;) {
		struct bp_motor_group_state state;
		err = bp_motor_group_get(&state);
		if (err) return err;
		if (state.request != request) return -ESTALE;
		if (!state.pending && !state.active) {
			/* This demonstration intentionally lets its lease expire. */
			if (state.error != -ETIMEDOUT) return state.error ? state.error : -EIO;
			return state.cleanup_error;
		}
		if (bp_time_ms() > command.deadline_ms + 100) return -ETIMEDOUT;
		err = bp_sleep(1);
		if (err) return err;
	}
}
