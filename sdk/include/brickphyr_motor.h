// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <brickphyr_port.h>

enum bp_motor_mode { BP_MOTOR_COAST, BP_MOTOR_DUTY };
struct bp_motor_state {
	/* Acceptance and completion times, monotonic ms. Completion is a driver
	 * result, not voltage/current feedback. Check valid and error. */
	int64_t requested_ms, applied_ms, deadline_ms;
	uint32_t generation, request, completed, failures;
	int32_t error, cleanup_error;
	int32_t requested_duty, duty; /* -1000..1000 permille; zero DUTY brakes */
	uint8_t requested_mode, mode, owner, pending, releasing, valid;
};
