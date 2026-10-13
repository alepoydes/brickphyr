// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

enum bp_charger_status { BP_CHARGER_DISCHARGING, BP_CHARGER_CHARGING, BP_CHARGER_COMPLETE, BP_CHARGER_FAULT };
enum bp_charger_source { BP_CHARGER_SOURCE_NONE, BP_CHARGER_SOURCE_NONSTANDARD,
	BP_CHARGER_SOURCE_SDP, BP_CHARGER_SOURCE_CDP, BP_CHARGER_SOURCE_DCP };
enum bp_charger_reason { BP_CHARGER_NO_USB, BP_CHARGER_WAIT_USB, BP_CHARGER_USB_SUSPENDED,
	BP_CHARGER_ACTIVE, BP_CHARGER_RESTART_PAUSE, BP_CHARGER_HARDWARE_FAULT, BP_CHARGER_IO_ERROR };

struct bp_charger_state {
	int64_t sample_ms;
	int32_t error, current_ma;
	int32_t source; /* bp_charger_source, or a negative detection/USB error. */
	/* Source allowance, not a measured input current or an ISET calibration. */
	uint32_t failures, input_budget_ma, faults, restarts;
	uint16_t current_raw;
	/* Last successfully applied ISET; error must be checked before trusting outputs. */
	uint8_t iset_percent, reason, status, enabled, paused, configured, suspended;
};
