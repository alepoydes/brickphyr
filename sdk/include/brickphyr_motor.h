// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <brickphyr_port.h>

enum bp_motor_mode { BP_MOTOR_COAST, BP_MOTOR_DUTY, BP_MOTOR_SPEED, BP_MOTOR_POSITION };
enum bp_motor_finish { BP_MOTOR_FINISH_COAST, BP_MOTOR_FINISH_BRAKE,
	BP_MOTOR_FINISH_HOLD, BP_MOTOR_FINISH_CONTINUE,
	BP_MOTOR_FINISH_COAST_SMART, BP_MOTOR_FINISH_BRAKE_SMART };
enum bp_motor_action { BP_MOTOR_TRAJECTORY, BP_MOTOR_TIMED, BP_MOTOR_RELATIVE,
	BP_MOTOR_TRACK, BP_MOTOR_HOLD, BP_MOTOR_UNTIL_STALLED };

struct bp_motor_command {
	int64_t target_mdeg; /* POSITION: physical angle, or displacement for RELATIVE; SPEED: zero */
	int32_t speed_mdeg_s; /* SPEED: signed; POSITION: positive, zero uses model limit */
	uint32_t duration_ms; /* SPEED: 1..299999; zero is forever except TIMED; POSITION: zero */
	uint8_t mode, finish, action;
	int16_t duty_limit; /* UNTIL_STALLED only: -1 uses control limit, else 0..1000 */
};

struct bp_motor_control_state {
	int64_t sample_ms, reference_mdeg;
	int32_t reference_speed, estimated_speed; /* millidegrees/second */
	int32_t requested_mv, applied_mv;
	uint32_t frames, overruns;
	uint8_t active, done, stalled, observing;
};

/* Shaft units, without direction/gearing. Load is a model estimate, not a
 * torque measurement. Speed uses the requested numerical derivative window. */
struct bp_motor_observation {
	int64_t sample_ms, angle_mdeg;
	int32_t speed_mdeg_s, estimated_speed, load_mnm, applied_mv;
	uint32_t generation, frames;
	int32_t error;
	uint8_t stalled;
};

/* Shared coordinates at the output of an external gear train. The ratio is
 * shaft millidegrees per output degree (1000 without gears), always positive.
 * Reset at setup uses the absolute shaft index when available, otherwise zero. */
struct bp_servo_setup {
	int32_t direction, gear_ratio, precision_profile;
	uint32_t reset_angle;
};

/* Shared servo settings in output degrees, degrees/s, degrees/s², mNm and ms.
 * PID gains retain PBIO's shaft units. Revision identifies an atomic update;
 * applied catches up at the next control frame. */
enum bp_servo_setting { BP_SERVO_LIMITS, BP_SERVO_PID, BP_SERVO_TARGET_TOLERANCES, BP_SERVO_STALL_TOLERANCES };
struct bp_servo_settings {
	uint32_t revision, applied;
	int32_t scale;
	int32_t speed, acceleration, deceleration, torque;
	int32_t kp, ki, kd, integral_deadzone, integral_rate;
	int32_t target_speed, target_position, stall_speed;
	uint32_t stall_time;
};

/* Pybricks trajectory diagnostics: relative shaft angles in degrees, speeds in
 * degrees/s, accelerations in degrees/s² and phase times in ms. Signs follow
 * the configured direction, without gearing. No controller pointers escape. */
struct bp_servo_trajectory {
	int32_t points[13], load;
	uint8_t active, done, stalled;
};

struct bp_servo_state {
	int64_t sample_ms;
	int32_t angle, speed, load; /* output degrees, degrees/s, shaft mNm */
	uint32_t request, completed;
	uint8_t pending, active, done, stalled;
};

enum bp_servo_action { BP_SERVO_RUN, BP_SERVO_TIME, BP_SERVO_ANGLE,
	BP_SERVO_TARGET, BP_SERVO_TRACK, BP_SERVO_HOLD, BP_SERVO_UNTIL_STALLED };
struct bp_servo_command {
	int32_t speed, angle; /* output degrees/s and degrees */
	uint32_t duration_ms;
	int16_t duty_limit; /* UNTIL_STALLED: -1 or 0..1000 permille */
	uint8_t action, finish;
};

struct bp_motor_state {
	/* Acceptance and completion times, monotonic ms. Completion is a driver
	 * result, not voltage/current feedback. Check valid and error. */
	int64_t requested_ms, applied_ms, deadline_ms;
	uint32_t generation, request, completed, failures;
	int32_t error, cleanup_error;
	int32_t requested_duty, duty; /* -1000..1000 permille; zero DUTY brakes */
	uint8_t requested_mode, mode, owner, pending, releasing, valid;
	/* DCMotor configuration, shared by objects on this connection. Direction
	 * is +1/-1; zero means no configured application motor. Limit is mV. */
	int8_t direction;
	int32_t max_voltage;
	int64_t zero_mdeg; /* physical shaft origin; raw observations stay unchanged */
	int32_t gear_ratio, precision_profile; /* zero ratio: no configured servo */
	struct bp_motor_command command;
	struct bp_motor_control_state control;
};
