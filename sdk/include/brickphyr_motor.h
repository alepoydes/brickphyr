// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <brickphyr_port.h>

enum bp_motor_mode { BP_MOTOR_COAST, BP_MOTOR_DUTY, BP_MOTOR_SPEED, BP_MOTOR_POSITION };
enum bp_motor_finish { BP_MOTOR_FINISH_COAST, BP_MOTOR_FINISH_BRAKE,
	BP_MOTOR_FINISH_HOLD, BP_MOTOR_FINISH_CONTINUE };

struct bp_motor_command {
	int64_t target_mdeg; /* POSITION: absolute encoder angle; SPEED: zero */
	int32_t speed_mdeg_s; /* SPEED: signed; POSITION: positive, zero uses model limit */
	uint32_t duration_ms; /* SPEED: 1..299999, zero runs until replaced; POSITION: zero */
	uint8_t mode, finish;
};

struct bp_motor_control_state {
	int64_t sample_ms, reference_mdeg;
	int32_t reference_speed, estimated_speed; /* millidegrees/second */
	int32_t requested_mv, applied_mv;
	uint32_t frames, overruns;
	uint8_t active, done, stalled;
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
	struct bp_motor_command command;
	struct bp_motor_control_state control;
};
