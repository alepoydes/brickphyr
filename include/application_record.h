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
/* Same layout/limits as begin, without allocating or changing a recording. */
int application_record_budget(size_t size, size_t capacity, size_t config_size,
	bool control, size_t parameter_size, size_t *bytes);
/* Variable payloads in a finite capture; includes the offset index and blocks. */
int application_record_compact_budget(size_t size, size_t capacity, size_t config_size,
	size_t payload_bytes, size_t *bytes);

/* Supervisor serializes reset/begin/clear with application termination. Reads
 * and fixed-size writes use only the bounded recorder lock, never lifecycle. */
void application_record_reset(void);
void application_record_end(void);
void application_record_clear(void);
int application_record_begin(const struct bp_record_spec *spec, const void *config, size_t length,
	bool control, size_t parameter_size);
/* Fixed-size stream retains every unacknowledged record. A full write returns
 * ENOSPC while the producer remains active; host acknowledgement frees slots. */
int application_record_begin_paused(const struct bp_record_spec *spec, const void *config,
	size_t length, bool control, size_t parameter_size);
/* A full segment pauses writes until host reads advance past its final row.
 * Advancing releases rows without a separate acknowledgement request. */
int application_record_begin_segments(const struct bp_record_spec *spec, const void *config,
	size_t length, bool control, size_t parameter_size);
/* A bounded variable-length ring for independently decodable binary blocks.
 * Each slot reserves spec.size bytes; actual lengths are published separately. */
int application_record_begin_variable_paused(const struct bp_record_spec *spec,
	const void *config, size_t length);
/* Fixed-capacity capture: no control metadata or overwrite. The final slot
 * completes with FULL or DURATION, as selected at admission. */
int application_record_begin_finite(const struct bp_record_spec *spec, const void *config,
	size_t length, uint32_t full_reason);
int application_record_begin_compact(const struct bp_record_spec *spec, const void *config,
	size_t length, size_t payload_bytes, uint32_t full_reason);
/* Finish a finite capture; an earlier FULL/finish reason remains authoritative. */
void application_record_finish(uint32_t reason);
/* Called by the control context after a successful control wait. */
void application_record_step(const struct bp_control_step *step, const void *parameters);
int application_record_write(const void *data, size_t length);
/* Publish the last finite-capture event and its actual reason atomically. */
int application_record_write_final(const void *data, size_t length, uint32_t reason);
/* Zero generation selects the caller's current run; host requests bind a run. */
int application_record_trigger(uint32_t generation, uint32_t after);
void application_record_info(struct bp_record_info *info);
/* Free packed payload bytes and record slots in the active finite recorder. */
size_t application_record_room(uint32_t *slots);
/* Host-supplied capture identity, retained until reset/clear. All-zero means
 * unbound. Binding an existing capture returns its original tag unchanged. */
#define APPLICATION_RECORD_TAG_SIZE 16
void application_record_snapshot(struct bp_record_info *info, uint8_t tag[APPLICATION_RECORD_TAG_SIZE],
	bool *variable, bool *paused, bool *segments);
int application_record_advance(uint32_t generation, uint64_t index);
int application_record_bind(uint32_t generation, const uint8_t candidate[APPLICATION_RECORD_TAG_SIZE],
	uint8_t tag[APPLICATION_RECORD_TAG_SIZE]);
/* Exclusive complete-record boundary. One management session owns retirement;
 * disconnection releases ownership but does not discard retained records. */
int application_record_ack(uint32_t generation, uint64_t next, uint32_t session);
void application_record_session_closed(uint32_t session);
int application_record_read(uint32_t generation, uint64_t index, size_t offset,
	void *data, size_t capacity, struct bp_record_info *info, struct bp_control_step *step);
/* Consecutive slots, each containing the optional 32-byte control step followed
 * by its payload. Compact captures instead prefix each payload with its uint16
 * little-endian length. offset includes the prefix; a block may end anywhere. */
int application_record_read_block(uint32_t generation, uint64_t index, size_t offset,
	void *data, size_t capacity, struct bp_record_info *info);
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
