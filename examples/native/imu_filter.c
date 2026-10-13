// SPDX-License-Identifier: Apache-2.0
#include <arm_math.h>
#include <brickphyr.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU, 2048);

int app_main(void)
{
	/* Average four distinct observed gyro samples. The snapshot API can skip
	 * acquisitions; this is not a fixed-rate filter of every IMU sample. */
	static const float32_t coefficients[] = {0.25f, 0.25f, 0.25f, 0.25f};
	float32_t history[4];
	arm_fir_instance_f32 filter;
	uint32_t sequence = 0, epoch = 0, count = 0;
	bool initialized = false;
	for (;;) {
		struct bp_imu_state sample;
		int err = bp_imu_get_state(&sample);
		if (err) {
			return err;
		}
		if (sample.error == -EINPROGRESS || sample.error == -EAGAIN) {
			initialized = false;
		} else if (sample.error) {
			return sample.error;
		} else if (!initialized || sample.raw.sequence != sequence || sample.epoch != epoch) {
			if (!initialized || sample.epoch != epoch) {
				arm_fir_init_f32(&filter, 4, coefficients, history, 1);
				count = 0;
				initialized = true;
			}
			sequence = sample.raw.sequence;
			epoch = sample.epoch;
			float32_t raw = sample.angular_velocity[2], filtered;
			arm_fir_f32(&filter, &raw, &filtered, 1);
			/* A complete window first, then at most one line per 20 observations.
			 * This keeps terminal traffic separate from the 10 ms polling cadence. */
			if (++count == 20) {
				char line[96];
				int length = snprintf(line, sizeof(line),
					"imu-dsp seq=%u epoch=%u raw=%ld filtered=%ld mdps\n",
					sequence, epoch, (long)raw, (long)filtered);
				if (length < 0 || (size_t)length >= sizeof(line)) {
					return -EOVERFLOW;
				}
				err = bp_write(line, length);
				if (err < 0) {
					return err;
				}
				/* Slow or absent readers may lose output; application status
				 * counts dropped bytes without stopping sensor processing. */
				count = 0;
			}
		}
		err = bp_sleep(10);
		if (err) {
			return err;
		}
	}
}
