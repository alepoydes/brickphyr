// SPDX-License-Identifier: Apache-2.0
#include "control_record.h"
#include <errno.h>
#include <stdbool.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU | BP_CAP_BATTERY | BP_CAP_PORTS, 2048);
static struct bp_control_frame frame;

/* Read-only periodic observation: no actuator commands and no numerical model.
 * Python may change the A/B selection with one little-endian uint32 parameter.
 * Without Python, the same ELF observes both ports. */
int app_main(void)
{
	struct {
		struct control_record input;
		uint32_t port_mask, reserved;
	} record = {0};
	struct bp_control_step step;
	_Static_assert(sizeof(record) == 184, "Control observation record layout");
	const struct bp_record_spec spec = {
		.size = sizeof(record), .capacity = 32, .schema = "brickphyr.control-loop/1",
	};
	const uint32_t period_us = 20000;
	int err = bp_control_record_begin(&spec, &period_us, sizeof(period_us));
	if (err) return err;
	uint32_t mask = 3;
	bool recording = true;
	for (;;) {
		/* 20 ms is this example's observation period, not a platform limit. */
		err = bp_control_wait(period_us, &step, &mask, sizeof(mask));
		if (err) return err;
		if ((step.parameter_size && step.parameter_size != sizeof(mask)) || (mask & ~3U)) return -EINVAL;
		err = bp_control_frame_get(mask, &frame);
		if (err) return err;
		control_record_fill(&record.input, &frame);
		record.port_mask = mask;
		if (recording) {
			err = bp_record_write(&record, sizeof(record));
			if (err == -ENOSPC) recording = false;
			else if (err) return err;
		}
	}
}
