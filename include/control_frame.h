// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <brickphyr_control.h>

int control_frame_get(uint32_t port_mask, struct bp_control_frame *frame);
int control_frame_imu_wait(uint32_t port_mask, uint32_t sequence, bool previous,
	uint32_t timeout_ms, struct bp_control_frame *frame);
