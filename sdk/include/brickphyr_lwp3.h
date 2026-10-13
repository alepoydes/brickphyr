// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

#define BP_LWP3_NAME_MAX 14
#define BP_LWP3_MESSAGE_MAX 20
#define BP_LWP3_PAIR 1

struct bp_lwp3_config {
	/* Scan timeout in ms, 0 means indefinite. Connection timeout is 5 s. */
	uint32_t timeout_ms;
	uint8_t kind, notifications, flags;
	char name[BP_LWP3_NAME_MAX + 1];
};

struct bp_lwp3_state {
	int32_t error;
	uint8_t connected, busy;
	/* Remote: left +, center, -; right +, center, -; green center. */
	uint8_t buttons;
	char name[BP_LWP3_NAME_MAX + 1];
};

struct bp_lwp3_message {
	uint8_t size;
	uint8_t data[BP_LWP3_MESSAGE_MAX];
};
