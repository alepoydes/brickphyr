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

enum bp_imu_frame_flags {
	BP_IMU_FRAME_AXES = 1,
	BP_IMU_FRAME_HEADING = 2,
	BP_IMU_CALIBRATION = 4,
};

enum bp_imu_setting {
	BP_IMU_GYRO_THRESHOLD, BP_IMU_ACCEL_THRESHOLD, BP_IMU_BIAS,
	BP_IMU_GYRO_SCALE, BP_IMU_ACCEL_CALIBRATION, BP_IMU_HEADING_CORRECTION,
	BP_IMU_SETTING_COUNT,
};

/* Same units and ordering as the shared IMU settings. Unused cells are zero.
 * Group sizes are 1, 1, 3, 3, 6, 1. Gravity is +X,+Y,+Z,-X,-Y,-Z.
 * Integer settings retain their selected value through runtime/save/reload. */
struct bp_imu_settings {
	int32_t values[BP_IMU_SETTING_COUNT][6];
};

/* Application coordinates use the Pybricks top_side/front_side convention.
 * Axes must be finite, nonzero and orthogonal; lengths are normalized.
 * AXES resets heading to zero unless HEADING supplies an explicit angle. */
struct bp_imu_command {
	uint32_t flags;
	float front[3];
	float top[3];
	float heading;
	/* CALIBRATION is exclusive of frame changes. Apply all selected groups
	 * atomically in RAM; bit i selects enum bp_imu_setting i. Never auto-save. */
	uint32_t settings_mask;
	struct bp_imu_settings settings;
};

struct bp_imu_application_state {
	struct bp_imu_state physical;
	float base[9]; /* Row-major physical-hub to application transform. */
	float rotation_uncalibrated[3]; /* Physical axes; gyro scale undone, bias retained. */
	float heading_1d;
	float heading_3d;
	uint32_t pending;
	int32_t command_error;
	struct bp_imu_settings settings;
};
