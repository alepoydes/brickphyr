// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

enum bp_light { BP_LIGHT_CENTER, BP_LIGHT_BLUETOOTH, BP_LIGHT_COUNT };

struct bp_light_state {
	int64_t applied_ms;
	int32_t error;
	uint32_t errors;
	uint8_t rgb[3];
	uint8_t owned;
	uint8_t pending;
	uint8_t visible;
	uint8_t valid;
};
