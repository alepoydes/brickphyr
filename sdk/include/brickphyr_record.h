// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Fixed-size application records. schema identifies the application's binary
 * format, including version and byte order; the firmware does not reinterpret it.
 * At most 1024 bytes per record and 32768 bytes total, allocated only on begin.
 * Configuration and optional control metadata share that total budget.
 */
struct bp_record_spec {
	uint32_t size, capacity;
	char schema[48]; /* NUL-terminated printable ASCII, nonempty. */
};

struct bp_record_info {
	struct bp_record_spec spec;
	uint32_t generation;
	uint64_t first, next;
	uint64_t trigger;
	uint32_t config_size, after, parameter_size;
	uint32_t reason;
	bool triggered;
	bool active;
	bool control;
};

/* Independent cursor; reading does not consume records for other readers. */
struct bp_record_request {
	uint64_t index;
	uint32_t generation;
	void *data, *parameters;
	size_t capacity, parameters_capacity;
};

enum bp_record_reason { BP_RECORD_OPEN, BP_RECORD_APPLICATION_END, BP_RECORD_TRIGGER_END };
