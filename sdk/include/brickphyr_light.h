// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

enum bp_light { BP_LIGHT_CENTER, BP_LIGHT_BLUETOOTH, BP_LIGHT_COUNT };

/* At most 2 KiB copied per animation submission. Durations are 1..INT32_MAX ms. */
#define BP_LIGHT_MAX_FRAMES 256
struct bp_light_frame {
	uint32_t duration_ms;
	uint8_t rgb[3];
};

struct bp_light_state {
	int64_t applied_ms;
	int32_t error;
	uint32_t errors;
	uint16_t frame;
	uint16_t frames;
	uint8_t rgb[3];
	uint8_t owned;
	uint8_t pending;
	uint8_t visible;
	uint8_t valid;
	uint8_t animating;
};
