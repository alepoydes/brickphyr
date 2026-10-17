// SPDX-License-Identifier: Apache-2.0
#include "application_record.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

static struct k_spinlock lock;
static struct bp_record_info state;
static uint8_t capture_tag[APPLICATION_RECORD_TAG_SIZE];
static uint8_t **records;
static uint8_t *configuration, *applied_parameters;
static size_t parameter_stride, step_size;
static unsigned int block_shift, block_mask;
static uint16_t free_parameters;
static size_t retained_bytes;
static size_t block_count, packed_capacity;
static uint16_t *offsets;
static uint16_t *lengths;
enum { PACKED_BLOCK_SIZE = 1024 };
static uint32_t full_reason;
static bool pause_on_full;
static uint32_t ack_session;
struct recorded_step {
	struct bp_control_step value;
	uint16_t parameters;
};
static struct bp_control_step current_step;
struct parameter_slot {
	uint16_t references, next;
	uint8_t data[];
};

static struct parameter_slot *parameter_slot(uint16_t index)
{
	size_t count = MIN(block_mask + 1, state.spec.capacity - (index & ~block_mask));
	return (void *)(records[index >> block_shift] + count * step_size +
		(index & block_mask) * parameter_stride);
}

static struct recorded_step *recorded_step(size_t index)
{
	return (struct recorded_step *)records[index >> block_shift] + (index & block_mask);
}

static uint8_t *record_data(size_t index)
{
	size_t count = MIN(block_mask + 1, state.spec.capacity - (index & ~block_mask));
	return records[index >> block_shift] + count * (step_size + parameter_stride) +
		(index & block_mask) * state.spec.size;
}

static size_t record_length(size_t index)
{
	return offsets ? offsets[index + 1] - offsets[index] :
		lengths ? lengths[index] : state.spec.size;
}

/* Packed records may cross allocation boundaries. No allocation or scan is
 * needed while sampling; the offset index also permits direct resume reads. */
static void packed_copy(size_t position, void *data, size_t length, bool write)
{
	while (length) {
		size_t part = MIN(length, PACKED_BLOCK_SIZE - position % PACKED_BLOCK_SIZE);
		uint8_t *stored = records[position / PACKED_BLOCK_SIZE] + position % PACKED_BLOCK_SIZE;
		if (write) memcpy(stored, data, part);
		else memcpy(data, stored, part);
		position += part;
		data = (uint8_t *)data + part;
		length -= part;
	}
}

static void copy_payload(size_t index, size_t offset, void *data, size_t length)
{
	if (offsets) packed_copy(offsets[index] + offset, data, length, false);
	else memcpy(data, record_data(index) + offset, length);
}

static void free_records(uint8_t **blocks, size_t count)
{
	for (size_t i = 0; i < count; i++) k_free(blocks[i]);
	k_free(blocks);
}

static void reset(bool active)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint8_t **old = records;
	size_t count = block_count;
	records = NULL;
	retained_bytes = 0;
	block_count = packed_capacity = 0;
	offsets = NULL;
	lengths = NULL;
	configuration = applied_parameters = NULL;
	current_step = (struct bp_control_step){0};
	full_reason = BP_RECORD_OPEN;
	pause_on_full = false;
	ack_session = 0;
	uint32_t generation = state.generation + 1;
	state = (struct bp_record_info){.generation = generation ? generation : 1,
		.active = active};
	memset(capture_tag, 0, sizeof(capture_tag));
	k_spin_unlock(&lock, key);
	free_records(old, count);
}

void application_record_clear(void)
{
	reset(false);
}

void application_record_reset(void)
{
	reset(true);
}

void application_record_end(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (state.active) state.reason = BP_RECORD_APPLICATION_END;
	state.active = false;
	k_spin_unlock(&lock, key);
}

struct record_layout {
	size_t step, stride, slot, count, bytes;
	unsigned int shift;
};

static int record_layout(size_t size, size_t capacity, size_t length, bool control,
	size_t parameter_size, struct record_layout *layout)
{
	if (!size || size > 1024 || !capacity ||
	    length > 1024 || parameter_size > 256 || (!control && parameter_size) ||
	    capacity > 32768 / size) return -EINVAL;
	size_t step_bytes = control ? sizeof(struct recorded_step) : 0;
	size_t stride = parameter_size ? ROUND_UP(sizeof(struct parameter_slot) + parameter_size, 4) : 0;
	size_t slot_bytes = step_bytes + stride + size;
	if (slot_bytes * capacity + parameter_size + length > 32768) return -EINVAL;
	/* Protected arenas and stacks can split otherwise sufficient free memory.
	 * Keep complete slots in small blocks; a power-of-two slot count gives
	 * constant-time lookup without padding each record to metadata alignment.
	 * One maximum-size record plus metadata may exceed the 1 KiB target. */
	unsigned int shift = 0;
	while ((2U << shift) <= 1024 / slot_bytes) shift++;
	size_t slots = 1U << shift;
	size_t count = DIV_ROUND_UP(capacity, slots);
	*layout = (struct record_layout){step_bytes, stride, slot_bytes, count,
		count * sizeof(*records) + parameter_size + length + slot_bytes * capacity, shift};
	return 0;
}

static int compact_layout(size_t size, size_t capacity, size_t length,
	size_t payload_bytes, struct record_layout *layout)
{
	if (!size || size > 1024 || !capacity || capacity > 32768 || length > 1024 ||
	    payload_bytes < capacity || payload_bytes > 32768 || payload_bytes > size * capacity) return -EINVAL;
	size_t count = DIV_ROUND_UP(payload_bytes, PACKED_BLOCK_SIZE);
	size_t bytes = count * sizeof(*records) + (capacity + 1) * sizeof(*offsets) +
		length + payload_bytes;
	if (bytes > 32768) return -EINVAL;
	*layout = (struct record_layout){.count = count, .bytes = bytes};
	return 0;
}

int application_record_budget(size_t size, size_t capacity, size_t length,
	bool control, size_t parameter_size, size_t *bytes)
{
	struct record_layout layout;
	int err = record_layout(size, capacity, length, control, parameter_size, &layout);
	if (!err) *bytes = layout.bytes;
	return err;
}

int application_record_compact_budget(size_t size, size_t capacity, size_t length,
	size_t payload_bytes, size_t *bytes)
{
	struct record_layout layout;
	int err = compact_layout(size, capacity, length, payload_bytes, &layout);
	if (!err) *bytes = layout.bytes;
	return err;
}

static int begin(const struct bp_record_spec *spec, const void *config, size_t length,
	bool control, size_t parameter_size, uint32_t completion_reason, size_t payload_bytes,
	bool paused, bool variable_ring)
{
	if (variable_ring && (!paused || control || payload_bytes)) return -EINVAL;
	struct record_layout layout;
	int err = payload_bytes ? compact_layout(spec->size, spec->capacity, length, payload_bytes, &layout) :
		record_layout(spec->size, spec->capacity, length, control, parameter_size, &layout);
	if (err) return err;
	if (variable_ring && layout.bytes + spec->capacity * sizeof(*lengths) > 32768) return -EINVAL;
	if (!spec->schema[0] || !memchr(spec->schema, 0, sizeof(spec->schema))) return -EINVAL;
	for (size_t i = 0; spec->schema[i]; i++) {
		if (spec->schema[i] < 32 || spec->schema[i] > 126) return -EINVAL;
	}
	/* The supervisor owns configuration and the caller is blocked. Allocate
	 * before publication; no allocator or transport runs inside the data lock. */
	if (records) return -EALREADY;
	size_t step_bytes = layout.step, stride = layout.stride, slot_bytes = layout.slot;
	size_t slots = 1U << layout.shift, count = layout.count;
	size_t index_size = payload_bytes ? (spec->capacity + 1) * sizeof(*offsets) :
		variable_ring ? spec->capacity * sizeof(*lengths) : 0;
	uint8_t **memory = k_malloc(count * sizeof(*memory) + index_size + parameter_size + length);
	if (!memory) return -ENOMEM;
	uint16_t *index = payload_bytes ? (void *)(memory + count) : NULL;
	uint16_t *variable_index = variable_ring ? (void *)(memory + count) : NULL;
	if (index) index[0] = 0;
	uint8_t *applied = (uint8_t *)(memory + count) + index_size;
	uint8_t *config_data = applied + parameter_size;
	if (length) memcpy(config_data, config, length);
	/* Pool slots hold only revisions referenced by retained records. The free
	 * list makes insertion/eviction constant-time without an allocator or scan. */
	for (size_t block = 0; block < count; block++) {
		size_t first = block * slots;
		size_t used = payload_bytes ? 0 : MIN(slots, spec->capacity - first);
		memory[block] = k_malloc(payload_bytes ?
			MIN(PACKED_BLOCK_SIZE, payload_bytes - block * PACKED_BLOCK_SIZE) : used * slot_bytes);
		if (!memory[block]) {
			free_records(memory, block);
			return -ENOMEM;
		}
		if (stride) {
			for (size_t i = 0; i < used; i++) {
				struct parameter_slot *slot = (void *)(memory[block] + used * step_bytes + i * stride);
				slot->references = 0;
				slot->next = first + i + 1 < spec->capacity ? first + i + 1 : UINT16_MAX;
			}
		}
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	state.spec = *spec;
	state.config_size = length;
	state.control = control;
	state.parameter_size = parameter_size;
	full_reason = completion_reason;
	pause_on_full = paused;
	ack_session = 0;
	step_size = step_bytes;
	parameter_stride = stride;
	block_shift = layout.shift;
	block_mask = slots - 1;
	applied_parameters = applied;
	configuration = config_data;
	free_parameters = 0;
	records = memory;
	block_count = count;
	packed_capacity = payload_bytes;
	offsets = index;
	lengths = variable_index;
	retained_bytes = layout.bytes + (variable_ring ? index_size : 0);
	k_spin_unlock(&lock, key);
	return 0;
}

int application_record_begin(const struct bp_record_spec *spec, const void *config, size_t length,
	bool control, size_t parameter_size)
{
	return begin(spec, config, length, control, parameter_size, BP_RECORD_OPEN, 0, false, false);
}

int application_record_begin_paused(const struct bp_record_spec *spec, const void *config,
	size_t length, bool control, size_t parameter_size)
{
	return begin(spec, config, length, control, parameter_size, BP_RECORD_OPEN, 0, true, false);
}

int application_record_begin_variable_paused(const struct bp_record_spec *spec,
	const void *config, size_t length)
{
	return begin(spec, config, length, false, 0, BP_RECORD_OPEN, 0, true, true);
}

int application_record_begin_finite(const struct bp_record_spec *spec, const void *config,
	size_t length, uint32_t reason)
{
	if (reason != BP_RECORD_FULL && reason != BP_RECORD_DURATION) return -EINVAL;
	return begin(spec, config, length, false, 0, reason, 0, false, false);
}

int application_record_begin_compact(const struct bp_record_spec *spec, const void *config,
	size_t length, size_t payload_bytes, uint32_t reason)
{
	if (!payload_bytes || (reason != BP_RECORD_FULL && reason != BP_RECORD_DURATION)) return -EINVAL;
	return begin(spec, config, length, false, 0, reason, payload_bytes, false, false);
}

void application_record_finish(uint32_t reason)
{
	__ASSERT_NO_MSG(reason == BP_RECORD_FULL || reason == BP_RECORD_DURATION || reason == BP_RECORD_CANCELLED ||
		reason == BP_RECORD_ERROR);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (full_reason != BP_RECORD_OPEN && state.active) {
		state.active = false;
		state.reason = reason;
	}
	k_spin_unlock(&lock, key);
}

size_t application_record_memory(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	size_t size = retained_bytes;
	k_spin_unlock(&lock, key);
	return size;
}

void application_record_step(const struct bp_control_step *step, const void *data)
{
	/* Only the native caller accesses this staging area; supervisor setup blocks
	 * that caller, and reset/free happens after joining it. Python updates its
	 * pending block separately. Host readers only see committed record slots. */
	if (!state.control) return;
	if ((!current_step.sequence || step->parameter_revision != current_step.parameter_revision) &&
	    state.parameter_size && step->parameter_size == state.parameter_size) {
		memcpy(applied_parameters, data, state.parameter_size);
	}
	current_step = *step;
}

static int write_record(const void *data, size_t length, uint32_t reason)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = !records ? -ENODATA : !state.active ?
		(state.reason == BP_RECORD_TRIGGER_END ||
			(full_reason != BP_RECORD_OPEN && state.next == state.spec.capacity) ?
			-ENOSPC : -ENODATA) :
		(!length || length > state.spec.size || (!offsets && !lengths && length != state.spec.size)) ?
			-EMSGSIZE :
		reason != BP_RECORD_OPEN && full_reason == BP_RECORD_OPEN ? -EINVAL :
		state.control && !current_step.sequence ? -ENODATA :
		state.control && current_step.parameter_size != state.parameter_size ? -EMSGSIZE :
		pause_on_full && state.next - state.first == state.spec.capacity ? -ENOSPC :
		full_reason != BP_RECORD_OPEN && state.next >= state.spec.capacity ? -ENOSPC :
		offsets && length > packed_capacity - offsets[state.next] ? -ENOSPC :
		state.next == UINT64_MAX ? -EOVERFLOW : 0;
	if (!err) {
		size_t index = state.next % state.spec.capacity;
		if (state.control) {
			uint16_t selected = UINT16_MAX;
			if (state.parameter_size) {
				if (state.next > state.first) {
					const struct recorded_step *previous = recorded_step((state.next - 1) % state.spec.capacity);
					if (previous->value.parameter_revision == current_step.parameter_revision) {
						selected = previous->parameters;
						parameter_slot(selected)->references++;
					}
				}
				if (!pause_on_full && state.next - state.first == state.spec.capacity) {
					uint16_t old = recorded_step(index)->parameters;
					struct parameter_slot *slot = parameter_slot(old);
					if (!--slot->references) {
						slot->next = free_parameters;
						free_parameters = old;
					}
				}
				if (selected == UINT16_MAX) {
					selected = free_parameters;
					struct parameter_slot *slot = parameter_slot(selected);
					free_parameters = slot->next;
					slot->references = 1;
					memcpy(slot->data, applied_parameters, state.parameter_size);
				}
			}
			*recorded_step(index) = (struct recorded_step){.value = current_step, .parameters = selected};
		}
		if (offsets) {
			packed_copy(offsets[index], (void *)data, length, true);
			offsets[index + 1] = offsets[index] + length;
		} else {
			memcpy(record_data(index), data, length);
			if (lengths) lengths[index] = length;
		}
		state.next++;
		if (reason != BP_RECORD_OPEN || (full_reason != BP_RECORD_OPEN && state.next == state.spec.capacity)) {
			state.active = false;
			state.reason = reason != BP_RECORD_OPEN ? reason : full_reason;
		} else if (state.next - state.first > state.spec.capacity) {
			state.first++;
		}
		if (state.active && state.triggered && state.next - state.trigger == state.after) {
			state.active = false;
			state.reason = BP_RECORD_TRIGGER_END;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

int application_record_write(const void *data, size_t length)
{
	return write_record(data, length, BP_RECORD_OPEN);
}

int application_record_write_final(const void *data, size_t length, uint32_t reason)
{
	if (reason != BP_RECORD_FULL && reason != BP_RECORD_DURATION &&
	    reason != BP_RECORD_CANCELLED && reason != BP_RECORD_ERROR) return -EINVAL;
	return write_record(data, length, reason);
}

int application_record_trigger(uint32_t generation, uint32_t after)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = generation && generation != state.generation ? -ESTALE :
		full_reason != BP_RECORD_OPEN || pause_on_full ? -ENOTSUP :
		state.triggered ? -EALREADY :
		!records || !state.active || !state.next ? -ENODATA :
		after >= state.spec.capacity ? -EINVAL :
		after > UINT64_MAX - state.next ? -EOVERFLOW : 0;
	if (!err) {
		state.triggered = true;
		state.trigger = state.next;
		state.after = after;
		if (!after) {
			state.active = false;
			state.reason = BP_RECORD_TRIGGER_END;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

void application_record_info(struct bp_record_info *info)
{
	application_record_snapshot(info, NULL, NULL, NULL);
}

size_t application_record_room(uint32_t *slots)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	size_t bytes = 0;
	*slots = 0;
	if (offsets && state.active && state.next < state.spec.capacity) {
		bytes = packed_capacity - offsets[state.next];
		*slots = state.spec.capacity - state.next;
	}
	k_spin_unlock(&lock, key);
	return bytes;
}

void application_record_snapshot(struct bp_record_info *info, uint8_t tag[APPLICATION_RECORD_TAG_SIZE],
	bool *variable, bool *paused)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	if (tag) memcpy(tag, capture_tag, sizeof(capture_tag));
	if (variable) *variable = offsets || lengths;
	if (paused) *paused = pause_on_full;
	k_spin_unlock(&lock, key);
}

int application_record_ack(uint32_t generation, uint64_t next, uint32_t session)
{
	if (!generation || !session) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = generation != state.generation ? -ESTALE : !records ? -ENODATA :
		!pause_on_full ? -ENOTSUP : ack_session && ack_session != session ? -EBUSY :
		next < state.first || next > state.next ? -EINVAL :
		next - state.first > 32 ? -E2BIG : 0;
	if (!err) {
		ack_session = session;
		while (state.first < next) {
			if (state.control && state.parameter_size) {
				uint16_t selected = recorded_step(state.first % state.spec.capacity)->parameters;
				struct parameter_slot *slot = parameter_slot(selected);
				if (!--slot->references) {
					slot->next = free_parameters;
					free_parameters = selected;
				}
			}
			state.first++;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

void application_record_session_closed(uint32_t session)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (ack_session == session) ack_session = 0;
	k_spin_unlock(&lock, key);
}

int application_record_bind(uint32_t generation, const uint8_t candidate[APPLICATION_RECORD_TAG_SIZE],
	uint8_t tag[APPLICATION_RECORD_TAG_SIZE])
{
	const uint8_t empty[APPLICATION_RECORD_TAG_SIZE] = {0};
	if (!memcmp(candidate, empty, sizeof(empty))) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = generation != state.generation ? -ESTALE : !records ? -ENODATA : 0;
	if (!err) {
		if (!memcmp(capture_tag, empty, sizeof(empty))) memcpy(capture_tag, candidate, sizeof(capture_tag));
		memcpy(tag, capture_tag, sizeof(capture_tag));
	}
	k_spin_unlock(&lock, key);
	return err;
}

int application_record_read(uint32_t generation, uint64_t index, size_t offset,
	void *data, size_t capacity, struct bp_record_info *info, struct bp_control_step *step)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	int length = 0;
	if (records && generation == state.generation && index >= state.first && index < state.next) {
		size_t slot = index % state.spec.capacity;
		if (offset >= record_length(slot)) {
			length = -EINVAL;
		} else {
			length = MIN(capacity, record_length(slot) - offset);
			copy_payload(slot, offset, data, length);
			if (state.control) *step = recorded_step(index % state.spec.capacity)->value;
		}
	}
	k_spin_unlock(&lock, key);
	return length;
}

int application_record_read_block(uint32_t generation, uint64_t index, size_t offset,
	void *data, size_t capacity, struct bp_record_info *info)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	size_t step = offsets || lengths ? 2 : state.control ? sizeof(struct bp_control_step) : 0;
	int length = 0;
	if (records && generation == state.generation && index >= state.first && index < state.next) {
		if (offset >= step + record_length(index % state.spec.capacity)) {
			length = -EINVAL;
		} else while (index < state.next && length < capacity) {
			size_t slot = index % state.spec.capacity;
			size_t stride = step + record_length(slot);
			if (offset < step) {
				size_t part = MIN(capacity - length, step - offset);
				uint8_t prefix[2];
				if (offsets || lengths) sys_put_le16(record_length(slot), prefix);
				const uint8_t *header = offsets || lengths ? prefix :
					(const uint8_t *)&recorded_step(slot)->value;
				memcpy((uint8_t *)data + length, header + offset, part);
				length += part;
				offset += part;
			}
			if (offset >= step && length < capacity) {
				size_t part = MIN(capacity - length, stride - offset);
				copy_payload(slot, offset - step, (uint8_t *)data + length, part);
				length += part;
				offset += part;
			}
			if (offset == stride) {
				index++;
				offset = 0;
			}
		}
	}
	k_spin_unlock(&lock, key);
	return length;
}

int application_record_parameters_read(uint32_t generation, uint64_t index,
	void *data, struct bp_record_info *info, uint32_t *revision)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	*revision = 0;
	int length = 0;
	if (records && generation == state.generation && index >= state.first && index < state.next) {
		if (!state.control) {
			length = -ENOTSUP;
		} else {
			const struct recorded_step *step = recorded_step(index % state.spec.capacity);
			*revision = step->value.parameter_revision;
			length = state.parameter_size;
			if (length) memcpy(data, parameter_slot(step->parameters)->data, length);
		}
	}
	k_spin_unlock(&lock, key);
	return length;
}

int application_record_copy(uint32_t generation, uint64_t index,
	void *data, size_t capacity, void *params, size_t params_capacity,
	struct bp_record_info *info, struct bp_control_step *step)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	int result = generation != state.generation ? -ESTALE : !records ? -ENODATA :
		index < state.first ? -EOVERFLOW : index > state.next ? -EINVAL :
		capacity < state.spec.size || params_capacity < state.parameter_size ? -EMSGSIZE : 0;
	if (!result && index < state.next) {
		size_t slot = index % state.spec.capacity;
		copy_payload(slot, 0, data, record_length(slot));
		if (state.control) {
			const struct recorded_step *stored = recorded_step(slot);
			*step = stored->value;
			if (state.parameter_size) {
				memcpy(params, parameter_slot(stored->parameters)->data, state.parameter_size);
			}
		}
		result = record_length(slot);
	}
	k_spin_unlock(&lock, key);
	return result;
}

int application_record_config_read(uint32_t generation, size_t offset,
	void *data, size_t capacity, struct bp_record_info *info)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	int length = 0;
	if (records && generation == state.generation && state.config_size) {
		if (offset >= state.config_size) {
			length = -EINVAL;
		} else {
			length = MIN(capacity, state.config_size - offset);
			memcpy(data, configuration + offset, length);
		}
	}
	k_spin_unlock(&lock, key);
	return length;
}
