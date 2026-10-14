// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

/* Fixed-size application records. schema identifies the application's binary
 * format, including version and byte order; the firmware does not reinterpret it.
 * At most 1024 bytes per record and 32768 bytes total, allocated only on begin.
 * Configuration and optional control metadata share that total budget.
 */
struct bp_record_spec {
	uint32_t size, capacity;
	char schema[48]; /* NUL-terminated printable ASCII, nonempty. */
};
