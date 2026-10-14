// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>
#include <stdbool.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU, 2048);

struct parameters { uint32_t mode, value; };
struct observation {
	int64_t sample_us;
	uint32_t raw_sequence, epoch, value;
	int32_t wait_error;
};
_Static_assert(sizeof(struct observation) == 24, "IMU trigger observation layout");

static struct bp_control_frame frame;

int app_main(void)
{
	const struct bp_record_spec spec = {
		.size = sizeof(struct observation), .capacity = 16,
		.schema = "fixture.control-imu/1",
	};
	int err = bp_control_record_begin(&spec, NULL, 0);
	if (err) return err;
	struct observation last = {0};
	bool checked_mode = false;
	bool retained = false;
	struct parameters parameters = {0};
	for (;;) {
		struct bp_control_step step;
		err = bp_control_imu_wait(0, parameters.mode == 3 ? 30000 : 30,
			&step, &frame, &parameters, sizeof(parameters));
		if (err) {
			/* A failed wait does not create another step. The error record is
			 * explicitly tagged and retains the preceding successful metadata. */
			if (last.raw_sequence) {
				if (!retained) (void)bp_record_write(&last, sizeof(last));
				last.wait_error = err;
				(void)bp_record_write(&last, sizeof(last));
			}
			return err;
		}
		if (step.parameter_size != sizeof(parameters) ||
		    !frame.imu.raw.sample_us || frame.imu.error ||
		    step.scheduled_us != frame.imu.raw.sample_us ||
		    frame.capture_end_us < frame.capture_start_us ||
		    frame.imu.raw.sequence == last.raw_sequence) return -EBADMSG;
		if (!checked_mode) {
			/* One native lifetime cannot change from sample to periodic release. */
			struct bp_control_step periodic;
			if (bp_control_wait(10000, &periodic, NULL, 0) != -EINVAL) return -EBADMSG;
			checked_mode = true;
		}
		last = (struct observation){
			.sample_us = frame.imu.raw.sample_us,
			.raw_sequence = frame.imu.raw.sequence,
			.epoch = frame.imu.epoch,
			.value = parameters.value,
		};
		/* In the fault exercise retain the first and last successful inputs,
		 * so host injection needs neither a guessed delay nor unbounded logs. */
		retained = parameters.mode == 0 || step.sequence == 1;
		if (retained) {
			err = bp_record_write(&last, sizeof(last));
			if (err) return err;
		}
		if (parameters.mode == 0) {
			if (step.sequence == 2) {
				err = bp_sleep(35); /* Let several real synthetic edges pass. */
				if (err) return err;
			}
			if (step.sequence == 8) return 0;
		}
	}
}
