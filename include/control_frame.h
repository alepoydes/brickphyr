// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <brickphyr_control.h>

int control_frame_get(uint32_t port_mask, struct bp_control_frame *frame);
int64_t control_time_us(void);
