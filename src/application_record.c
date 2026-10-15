// SPDX-License-Identifier: Apache-2.0
#include "application_record.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>

static struct k_spinlock lock;
static struct bp_record_info state;
static uint8_t **records;
static uint8_t *configuration, *applied_parameters;
static size_t parameter_stride, step_size;
static unsigned int block_shift, block_mask;
static uint16_t free_parameters;
static size_t retained_bytes;
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

static void free_records(uint8_t **blocks, size_t count)
{
	for (size_t i = 0; i < count; i++) k_free(blocks[i]);
	k_free(blocks);
}

static void reset(bool active)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint8_t **old = records;
	size_t count = old ? DIV_ROUND_UP(state.spec.capacity, block_mask + 1) : 0;
	records = NULL;
	retained_bytes = 0;
	configuration = applied_parameters = NULL;
	current_step = (struct bp_control_step){0};
	uint32_t generation = state.generation + 1;
	state = (struct bp_record_info){.generation = generation ? generation : 1,
		.active = active};
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

int application_record_budget(size_t size, size_t capacity, size_t length,
	bool control, size_t parameter_size, size_t *bytes)
{
	struct record_layout layout;
	int err = record_layout(size, capacity, length, control, parameter_size, &layout);
	if (!err) *bytes = layout.bytes;
	return err;
}

int application_record_begin(const struct bp_record_spec *spec, const void *config, size_t length,
	bool control, size_t parameter_size)
{
	struct record_layout layout;
	int err = record_layout(spec->size, spec->capacity, length, control, parameter_size, &layout);
	if (err) return err;
	if (!spec->schema[0] || !memchr(spec->schema, 0, sizeof(spec->schema))) return -EINVAL;
	for (size_t i = 0; spec->schema[i]; i++) {
		if (spec->schema[i] < 32 || spec->schema[i] > 126) return -EINVAL;
	}
	/* The supervisor owns configuration and the caller is blocked. Allocate
	 * before publication; no allocator or transport runs inside the data lock. */
	if (records) return -EALREADY;
	size_t step_bytes = layout.step, stride = layout.stride, slot_bytes = layout.slot;
	size_t slots = 1U << layout.shift, count = layout.count;
	uint8_t **memory = k_malloc(count * sizeof(*memory) + parameter_size + length);
	if (!memory) return -ENOMEM;
	uint8_t *applied = (void *)(memory + count);
	uint8_t *config_data = applied + parameter_size;
	if (length) memcpy(config_data, config, length);
	/* Pool slots hold only revisions referenced by retained records. The free
	 * list makes insertion/eviction constant-time without an allocator or scan. */
	for (size_t block = 0; block < count; block++) {
		size_t first = block * slots;
		size_t used = MIN(slots, spec->capacity - first);
		memory[block] = k_malloc(used * slot_bytes);
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
	step_size = step_bytes;
	parameter_stride = stride;
	block_shift = layout.shift;
	block_mask = slots - 1;
	applied_parameters = applied;
	configuration = config_data;
	free_parameters = 0;
	records = memory;
	retained_bytes = layout.bytes;
	k_spin_unlock(&lock, key);
	return 0;
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

int application_record_write(const void *data, size_t length)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = !records ? -ENODATA : !state.active ?
		(state.reason == BP_RECORD_TRIGGER_END ? -ENOSPC : -ENODATA) :
		length != state.spec.size ? -EMSGSIZE :
		state.control && !current_step.sequence ? -ENODATA :
		state.control && current_step.parameter_size != state.parameter_size ? -EMSGSIZE :
		state.next == UINT64_MAX ? -EOVERFLOW : 0;
	if (!err) {
		size_t index = state.next % state.spec.capacity;
		if (state.control) {
			uint16_t selected = UINT16_MAX;
			if (state.parameter_size) {
				if (state.next) {
					const struct recorded_step *previous = recorded_step((state.next - 1) % state.spec.capacity);
					if (previous->value.parameter_revision == current_step.parameter_revision) {
						selected = previous->parameters;
						parameter_slot(selected)->references++;
					}
				}
				if (state.next - state.first == state.spec.capacity) {
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
		memcpy(record_data(index), data, length);
		state.next++;
		if (state.next - state.first > state.spec.capacity) state.first++;
		if (state.triggered && state.next - state.trigger == state.after) {
			state.active = false;
			state.reason = BP_RECORD_TRIGGER_END;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

int application_record_trigger(uint32_t generation, uint32_t after)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = generation && generation != state.generation ? -ESTALE :
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
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	k_spin_unlock(&lock, key);
}

int application_record_read(uint32_t generation, uint64_t index, size_t offset,
	void *data, size_t capacity, struct bp_record_info *info, struct bp_control_step *step)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	int length = 0;
	if (records && generation == state.generation && index >= state.first && index < state.next) {
		if (offset >= state.spec.size) {
			length = -EINVAL;
		} else {
			length = MIN(capacity, state.spec.size - offset);
			memcpy(data, record_data(index % state.spec.capacity) + offset, length);
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
	size_t step = state.control ? sizeof(struct bp_control_step) : 0;
	size_t stride = step + state.spec.size;
	int length = 0;
	if (records && generation == state.generation && index >= state.first && index < state.next) {
		if (offset >= stride) {
			length = -EINVAL;
		} else while (index < state.next && length < capacity) {
			size_t slot = index % state.spec.capacity;
			if (offset < step) {
				size_t part = MIN(capacity - length, step - offset);
				memcpy((uint8_t *)data + length,
					(const uint8_t *)&recorded_step(slot)->value + offset, part);
				length += part;
				offset += part;
			}
			if (offset >= step && length < capacity) {
				size_t part = MIN(capacity - length, stride - offset);
				memcpy((uint8_t *)data + length, record_data(slot) + offset - step, part);
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
		memcpy(data, record_data(slot), state.spec.size);
		if (state.control) {
			const struct recorded_step *stored = recorded_step(slot);
			*step = stored->value;
			if (state.parameter_size) {
				memcpy(params, parameter_slot(stored->parameters)->data, state.parameter_size);
			}
		}
		result = state.spec.size;
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
