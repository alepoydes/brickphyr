// SPDX-License-Identifier: Apache-2.0
#include "application_record.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>

static struct k_spinlock lock;
static struct application_record_info state;
static uint8_t *records;
static uint8_t *configuration, *parameters, *applied_parameters;
static size_t parameter_stride;
static uint16_t free_parameters;
static struct recorded_step {
	struct bp_control_step value;
	uint16_t parameters;
} *steps;
static struct bp_control_step current_step;
struct parameter_slot {
	uint16_t references, next;
	uint8_t data[];
};

static struct parameter_slot *parameter_slot(uint16_t index)
{
	return (struct parameter_slot *)(parameters + index * parameter_stride);
}

static void reset(bool active)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint8_t *old = records;
	records = NULL;
	steps = NULL;
	configuration = parameters = applied_parameters = NULL;
	current_step = (struct bp_control_step){0};
	uint32_t generation = state.generation + 1;
	state = (struct application_record_info){.generation = generation ? generation : 1,
		.active = active};
	k_spin_unlock(&lock, key);
	k_free(old);
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
	if (state.active) state.reason = RECORD_APPLICATION_END;
	state.active = false;
	k_spin_unlock(&lock, key);
}

int application_record_begin(const struct bp_record_spec *spec, const void *config, size_t length,
	bool control, size_t parameter_size)
{
	if (!spec->size || spec->size > 1024 || !spec->capacity ||
	    length > 1024 || parameter_size > 256 || (!control && parameter_size) ||
	    spec->capacity > 32768 / spec->size || !spec->schema[0] ||
	    !memchr(spec->schema, 0, sizeof(spec->schema))) return -EINVAL;
	for (size_t i = 0; spec->schema[i]; i++) {
		if (spec->schema[i] < 32 || spec->schema[i] > 126) return -EINVAL;
	}
	/* The supervisor owns configuration and the caller is blocked. Allocate
	 * before publication; no allocator or transport runs inside the data lock. */
	if (records) return -EALREADY;
	size_t bytes = spec->size * spec->capacity;
	size_t step_offset = control ? ROUND_UP(bytes, __alignof__(*steps)) : bytes;
	size_t parameter_offset = step_offset + (control ? spec->capacity * sizeof(*steps) : 0);
	size_t stride = parameter_size ? ROUND_UP(sizeof(struct parameter_slot) + parameter_size, 4) : 0;
	size_t applied_offset = parameter_offset + spec->capacity * stride;
	size_t config_offset = applied_offset + parameter_size;
	if (config_offset + length > 32768) return -EINVAL;
	uint8_t *memory = k_malloc(config_offset + length);
	if (!memory) return -ENOMEM;
	if (length) memcpy(memory + config_offset, config, length);
	/* Pool slots hold only revisions referenced by retained records. The free
	 * list makes insertion/eviction constant-time without an allocator or scan. */
	if (stride) {
		for (size_t i = 0; i < spec->capacity; i++) {
			struct parameter_slot *slot = (void *)(memory + parameter_offset + i * stride);
			slot->references = 0;
			slot->next = i + 1 < spec->capacity ? i + 1 : UINT16_MAX;
		}
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	state.spec = *spec;
	state.config_size = length;
	state.control = control;
	state.parameter_size = parameter_size;
	steps = control ? (void *)(memory + step_offset) : NULL;
	parameters = memory + parameter_offset;
	parameter_stride = stride;
	applied_parameters = memory + applied_offset;
	configuration = memory + config_offset;
	free_parameters = 0;
	records = memory;
	k_spin_unlock(&lock, key);
	return 0;
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
		(state.reason == RECORD_TRIGGER_END ? -ENOSPC : -ENODATA) :
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
					const struct recorded_step *previous = &steps[(state.next - 1) % state.spec.capacity];
					if (previous->value.parameter_revision == current_step.parameter_revision) {
						selected = previous->parameters;
						parameter_slot(selected)->references++;
					}
				}
				if (state.next - state.first == state.spec.capacity) {
					uint16_t old = steps[index].parameters;
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
			steps[index] = (struct recorded_step){.value = current_step, .parameters = selected};
		}
		memcpy(records + index * state.spec.size, data, length);
		state.next++;
		if (state.next - state.first > state.spec.capacity) state.first++;
		if (state.triggered && state.next - state.trigger == state.after) {
			state.active = false;
			state.reason = RECORD_TRIGGER_END;
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
			state.reason = RECORD_TRIGGER_END;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

void application_record_info(struct application_record_info *info)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	k_spin_unlock(&lock, key);
}

int application_record_read(uint32_t generation, uint64_t index, size_t offset,
	void *data, size_t capacity, struct application_record_info *info, struct bp_control_step *step)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	int length = 0;
	if (records && generation == state.generation && index >= state.first && index < state.next) {
		if (offset >= state.spec.size) {
			length = -EINVAL;
		} else {
			length = MIN(capacity, state.spec.size - offset);
			memcpy(data, records + (index % state.spec.capacity) * state.spec.size + offset, length);
			if (state.control) *step = steps[index % state.spec.capacity].value;
		}
	}
	k_spin_unlock(&lock, key);
	return length;
}

int application_record_parameters_read(uint32_t generation, uint64_t index,
	void *data, struct application_record_info *info, uint32_t *revision)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	*revision = 0;
	int length = 0;
	if (records && generation == state.generation && index >= state.first && index < state.next) {
		if (!state.control) {
			length = -ENOTSUP;
		} else {
			const struct recorded_step *step = &steps[index % state.spec.capacity];
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
	struct application_record_info *info, struct bp_control_step *step)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*info = state;
	int result = generation != state.generation ? -ESTALE : !records ? -ENODATA :
		index < state.first ? -EOVERFLOW : index > state.next ? -EINVAL :
		capacity < state.spec.size || params_capacity < state.parameter_size ? -EMSGSIZE : 0;
	if (!result && index < state.next) {
		size_t slot = index % state.spec.capacity;
		memcpy(data, records + slot * state.spec.size, state.spec.size);
		if (state.control) {
			*step = steps[slot].value;
			if (state.parameter_size) {
				memcpy(params, parameter_slot(steps[slot].parameters)->data, state.parameter_size);
			}
		}
		result = state.spec.size;
	}
	k_spin_unlock(&lock, key);
	return result;
}

int application_record_config_read(uint32_t generation, size_t offset,
	void *data, size_t capacity, struct application_record_info *info)
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
