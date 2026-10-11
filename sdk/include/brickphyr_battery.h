// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

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
};
