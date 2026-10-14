// SPDX-License-Identifier: Apache-2.0
#include "control_record.h"
#include <errno.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU | BP_CAP_BATTERY | BP_CAP_PORTS, 2048);
static struct bp_control_frame frame;

/* Read-only periodic observation: no actuator commands and no numerical model.
 * Python may change the A/B selection with one little-endian uint32 parameter.
 * Without Python, the same ELF observes both ports. */
int app_main(void)
{
	struct {
		struct bp_control_step step;
		struct control_record input;
		uint32_t port_mask, reserved;
	} record = {0};
	_Static_assert(sizeof(record) == 216, "Control step record layout");
	const struct bp_record_spec spec = {
		.size = sizeof(record), .capacity = 32, .schema = "brickphyr.control-step/1",
	};
	int err = bp_record_begin(&spec);
	if (err) return err;
	uint32_t mask = 3;
	for (;;) {
		/* 20 ms is this example's observation period, not a platform limit. */
		err = bp_control_wait(20000, &record.step, &mask, sizeof(mask));
		if (err) return err;
		if ((record.step.parameter_size && record.step.parameter_size != sizeof(mask)) || (mask & ~3U)) return -EINVAL;
		err = bp_control_frame_get(mask, &frame);
		if (err) return err;
		control_record_fill(&record.input, &frame);
		record.port_mask = mask;
		err = bp_record_write(&record, sizeof(record));
		if (err) return err;
	}
}
