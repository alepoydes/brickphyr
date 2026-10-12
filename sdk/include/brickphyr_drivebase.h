// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

/* The two already configured servos must have identical gearing. Dimensions
 * are micrometres; Spike geometry uses average/difference shaft degrees. */
struct bp_drivebase_setup {
	uint32_t ports[2], generations[2];
	int32_t wheel_um, track_um;
	uint32_t spike;
};

enum bp_drivebase_action { BP_DRIVEBASE_RELATIVE, BP_DRIVEBASE_TURN,
	BP_DRIVEBASE_TIMED, BP_DRIVEBASE_COAST, BP_DRIVEBASE_BRAKE, BP_DRIVEBASE_RESET,
	BP_DRIVEBASE_GYRO, BP_DRIVEBASE_ARC_ANGLE, BP_DRIVEBASE_ARC_DISTANCE,
	BP_DRIVEBASE_CURVE, BP_DRIVEBASE_TANK_ANGLE, BP_DRIVEBASE_TANK_TIME,
	BP_DRIVEBASE_STEERING_ANGLE, BP_DRIVEBASE_STEERING_TIME, BP_DRIVEBASE_VECTOR };
struct bp_drivebase_command {
	int32_t distance, angle, speed, turn_rate; /* mm, deg, mm/s, deg/s */
	int32_t radius; /* signed mm for ARC/CURVE */
	int32_t steering; /* STEERING: -100..100; speed in wheel deg/s */
	/* TANK uses speed/turn_rate as left/right deg/s and angle as wheel deg. */
	/* VECTOR uses distance/angle as dx/dy mm: absolute heading, then straight. */
	uint32_t duration_ms; /* TIMED: 300000 means indefinite CONTINUE */
	uint32_t action, finish; /* bp_motor_finish */
	uint32_t gyro; /* GYRO: 0 encoders, 1 application-frame 3D heading */
};
struct bp_drivebase_settings {
	int32_t speed, acceleration, deceleration;
	int32_t turn_rate, turn_acceleration, turn_deceleration;
};
struct bp_drivebase_state {
	int64_t sample_ms;
	int32_t distance, speed, heading, turn_rate;
	float angle;
	uint32_t request, completed, motion; /* settings do not replace motion */
	int32_t error, command_error, cleanup_error;
	uint8_t pending, active, done, stalled, valid;
	uint8_t gyro;
	struct bp_drivebase_settings settings;
};
