// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

enum bp_battery_type {
	BP_BATTERY_UNKNOWN,
	BP_BATTERY_ALKALINE,
	BP_BATTERY_LI_ION,
};

/* Current is the measured load current, not signed charging current or state
 * of charge. Two sequential conversions form one observation; sample_ms marks
 * its completion. Errors retain the last valid pair and its timestamp.
 * read_ms is the last attempt's completion. Counters saturate except sequence.
 */
struct bp_battery_sample {
	int64_t sample_ms, read_ms;
	uint32_t voltage_mv, current_ma;
	uint32_t sequence, failures;
	int32_t error;
	uint16_t voltage_raw, current_raw;
	/* Chemistry is a board property, independent of acquisition errors.
	 * Temperature has its own validity: a failed thermistor must not invalidate
	 * the voltage/current pair used by motor control. Retain its last valid
	 * value and timestamp on failure. Negative Celsius values are supported. */
	uint32_t type;
	int32_t temperature_mdegc, temperature_error;
	int64_t temperature_ms;
	uint32_t temperature_failures;
	uint16_t temperature_raw;
};
