// SPDX-License-Identifier: Apache-2.0
#include "control_record.h"
#include <errno.h>
#include <string.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU | BP_CAP_BATTERY | BP_CAP_PORTS, 2048);

/* A native primary consumes observations while control_loop runs independently.
 * This example changes the observed port selection; it never drives motors.
 * An application can use the same exchange for its own numerical model. */
int app_main(void)
{
	uint32_t mask = 3;
	int err = bp_control_start_file("control_loop", sizeof("control_loop") - 1, &mask, sizeof(mask));
	if (err) return err;
	struct { struct control_record input; uint32_t port_mask, reserved; } record;
	struct bp_record_request request = {
		.data = &record, .capacity = sizeof(record),
		.parameters = &mask, .parameters_capacity = sizeof(mask),
	};
	struct bp_record_info info;
	uint32_t count = 0, revision = 0;
	while (count < 24) {
		err = bp_record_get(&info);
		if (err == -ENODATA) { bp_sleep(1); continue; }
		if (err) return err;
		request.generation = info.generation;
		if (request.index < info.first) return -EOVERFLOW;
		struct bp_control_step step;
		err = bp_record_readinto(&request, &info, &step);
		if (!err) { bp_sleep(1); continue; }
		if (err < 0) return err;
		if (err != sizeof(record) || mask != record.port_mask || record.input.errors[0]) return -EBADMSG;
		request.index++;
		count++;
		if (count == 8) {
			/* Apply the complete new selection at a controller step boundary. */
			mask = 0;
			err = bp_control_update(&mask, sizeof(mask));
			if (err < 0) return err;
			revision = err;
		}
		if (count == 24 && step.parameter_revision != revision) return -EAGAIN;
	}
	const char *message = "Native background read 24 control observations\n";
	bp_write(message, strlen(message));
	return 0;
}
