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

/* One coherent calibrated observation, in the same hub axes as raw.
 * Acceleration includes gravity; integer vectors retain raw's physical units.
 * orientation is a row-major body-to-inertial rotation matrix. Integrated
 * rotations and headings use degrees, with clockwise-positive heading.
 * epoch changes on initialization, missing data or calibration changes that
 * establish a new orientation reference.
 * Check error and epoch before comparing angles between observations.
 * ready means a recent stationary bias estimate, not guaranteed absolute rest.
 */
struct bp_imu_state {
	struct bp_imu_sample raw;
	int32_t acceleration[3];
	int32_t angular_velocity[3];
	int32_t bias[3];
	float orientation[9];
	float rotation[3];
	float heading_1d;
	float heading_3d;
	uint32_t epoch;
	uint32_t stationary;
	uint32_t ready;
	int32_t error;
};
