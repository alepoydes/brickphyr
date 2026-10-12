// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

enum bp_charger_status { BP_CHARGER_DISCHARGING, BP_CHARGER_CHARGING, BP_CHARGER_COMPLETE, BP_CHARGER_FAULT };
enum bp_charger_source { BP_CHARGER_SOURCE_NONE, BP_CHARGER_SOURCE_NONSTANDARD,
	BP_CHARGER_SOURCE_SDP, BP_CHARGER_SOURCE_CDP, BP_CHARGER_SOURCE_DCP };

struct bp_charger_state {
	int64_t sample_ms;
	int32_t error, current_ma;
	int32_t source; /* bp_charger_source, or a negative detection/USB error. */
	uint32_t failures, limit_ma;
	uint16_t current_raw;
	uint8_t status, enabled, paused, configured, suspended;
};
