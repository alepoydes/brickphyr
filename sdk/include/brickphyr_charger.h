// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

enum bp_charger_status { BP_CHARGER_DISCHARGING, BP_CHARGER_CHARGING, BP_CHARGER_COMPLETE, BP_CHARGER_FAULT };

struct bp_charger_state {
	int64_t sample_ms;
	int32_t error, current_ma;
	uint32_t failures, limit_ma;
	uint16_t current_raw;
	uint8_t status, enabled, paused, configured, suspended;
};
