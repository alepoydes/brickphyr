// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <brickphyr_record.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct application_record_info {
	struct bp_record_spec spec;
	uint32_t generation;
	uint64_t first, next;
	bool active;
};

/* Supervisor serializes reset/begin/clear with application termination. Reads
 * and fixed-size writes use only the bounded recorder lock, never lifecycle. */
void application_record_reset(void);
void application_record_end(void);
void application_record_clear(void);
int application_record_begin(const struct bp_record_spec *spec);
int application_record_write(const void *data, size_t length);
void application_record_info(struct application_record_info *info);
int application_record_read(uint32_t generation, uint64_t index, size_t offset,
	void *data, size_t capacity, struct application_record_info *info);
