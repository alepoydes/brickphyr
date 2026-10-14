// SPDX-License-Identifier: Apache-2.0
#include "control_record.h"

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU | BP_CAP_BATTERY | BP_CAP_PORTS, 2048);

/* Read-only observation, not a controller or a universal sample rate. */
static struct bp_control_frame frame;

int app_main(void)
{
	const struct bp_record_spec spec = {
		.size = sizeof(struct control_record), .capacity = 32, .schema = "brickphyr.control-input/1",
	};
	int err = bp_record_begin(&spec);
	if (err) return err;
	int64_t due = bp_time_ms();
	for (unsigned int i = 0; i < 40; i++) {
		err = bp_control_frame_get(3, &frame);
		if (err) return err;
		struct control_record record;
		control_record_fill(&record, &frame);
		err = bp_record_write(&record, sizeof(record));
		if (err) return err;
		if (i == 39) break;
		due += 20;
		int64_t now = bp_time_ms();
		if (due <= now) due += ((now - due) / 20 + 1) * 20;
		err = bp_sleep(due - now);
		if (err) return err;
	}
	return 0;
}
