// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <brickphyr_control.h>
#include <brickphyr_record.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Requested bytes retained by the recorder, including its block index and
 * parameter metadata, excluding allocator overhead. */
size_t application_record_memory(void);

/* Supervisor serializes reset/begin/clear with application termination. Reads
 * and fixed-size writes use only the bounded recorder lock, never lifecycle. */
void application_record_reset(void);
void application_record_end(void);
void application_record_clear(void);
int application_record_begin(const struct bp_record_spec *spec, const void *config, size_t length,
	bool control, size_t parameter_size);
/* Called by the control context after a successful control wait. */
void application_record_step(const struct bp_control_step *step, const void *parameters);
int application_record_write(const void *data, size_t length);
/* Zero generation selects the caller's current run; host requests bind a run. */
int application_record_trigger(uint32_t generation, uint32_t after);
void application_record_info(struct bp_record_info *info);
int application_record_read(uint32_t generation, uint64_t index, size_t offset,
	void *data, size_t capacity, struct bp_record_info *info, struct bp_control_step *step);
int application_record_parameters_read(uint32_t generation, uint64_t index,
	void *data, struct bp_record_info *info, uint32_t *revision);
/* One complete observation and its applied parameters, atomically copied.
 * Zero means the next record is not written yet. A stale generation, overwritten
 * index or undersized buffer fails before copying either payload. */
int application_record_copy(uint32_t generation, uint64_t index,
	void *data, size_t capacity, void *params, size_t params_capacity,
	struct bp_record_info *info, struct bp_control_step *step);
int application_record_config_read(uint32_t generation, size_t offset,
	void *data, size_t capacity, struct bp_record_info *info);
