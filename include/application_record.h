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
	uint64_t trigger;
	uint32_t config_size, after;
	enum { RECORD_OPEN, RECORD_APPLICATION_END, RECORD_TRIGGER_END } reason;
	bool triggered;
	bool active;
};

/* Supervisor serializes reset/begin/clear with application termination. Reads
 * and fixed-size writes use only the bounded recorder lock, never lifecycle. */
void application_record_reset(void);
void application_record_end(void);
void application_record_clear(void);
int application_record_begin(const struct bp_record_spec *spec, const void *config, size_t length);
int application_record_write(const void *data, size_t length);
/* Zero generation selects the caller's current run; host requests bind a run. */
int application_record_trigger(uint32_t generation, uint32_t after);
void application_record_info(struct application_record_info *info);
int application_record_read(uint32_t generation, uint64_t index, size_t offset,
	void *data, size_t capacity, struct application_record_info *info);
int application_record_config_read(uint32_t generation, size_t offset,
	void *data, size_t capacity, struct application_record_info *info);
