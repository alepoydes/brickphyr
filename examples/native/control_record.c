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
#ifdef BP_CONTROL_IMU
	/* Observe exactly the samples which release the native context. */
	const uint32_t timeout_ms = 100;
	int err = bp_control_record_begin(&spec, &timeout_ms, sizeof(timeout_ms));
	struct bp_control_step step;
#else
	int err = bp_record_begin(&spec);
	int64_t due = bp_time_ms();
#endif
	if (err) return err;
	for (unsigned int i = 0; i < 40; i++) {
#ifdef BP_CONTROL_IMU
		err = bp_control_imu_wait(3, timeout_ms, &step, &frame, NULL, 0);
#else
		err = bp_control_frame_get(3, &frame);
#endif
		if (err) return err;
		struct control_record record;
		control_record_fill(&record, &frame);
		err = bp_record_write(&record, sizeof(record));
		if (err) return err;
#ifndef BP_CONTROL_IMU
		if (i == 39) break;
		due += 20;
		int64_t now = bp_time_ms();
		if (due <= now) due += ((now - due) / 20 + 1) * 20;
		err = bp_sleep(due - now);
		if (err) return err;
#endif
	}
	return 0;
}
