// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

/* Uncalibrated, in Pybricks hub axes: +X front, +Y left, +Z top.
 * Acceleration is micrometres/s²; angular velocity is millidegrees/s.
 * The acquisition timestamp is the observed data-ready edge, not host reception.
 * An error preserves the last valid sample and its sequence/time. read_us is
 * the last acquisition attempt's completion. Counters saturate except sequence.
 */
struct bp_imu_sample {
	int64_t sample_us;
	int64_t read_us;
	int32_t acceleration[3];
	int32_t angular_velocity[3];
	uint32_t sequence;
	uint32_t missed;
	uint32_t failures;
	int32_t error;
};
