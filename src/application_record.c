// SPDX-License-Identifier: Apache-2.0
#include "application_record.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>

static struct k_spinlock lock;
static struct application_record_info state;
static uint8_t *records;

static void reset(bool active)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint8_t *old = records;
	records = NULL;
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

int application_record_begin(const struct bp_record_spec *spec, const void *config, size_t length)
{
	if (!spec->size || spec->size > 1024 || !spec->capacity ||
	    length > 1024 || spec->capacity > (32768 - length) / spec->size || !spec->schema[0] ||
	    !memchr(spec->schema, 0, sizeof(spec->schema))) return -EINVAL;
	for (size_t i = 0; spec->schema[i]; i++) {
		if (spec->schema[i] < 32 || spec->schema[i] > 126) return -EINVAL;
	}
	/* The supervisor owns configuration and the caller is blocked. Allocate
	 * before publication; no allocator or transport runs inside the data lock. */
	if (records) return -EALREADY;
	size_t bytes = spec->size * spec->capacity;
	uint8_t *memory = k_malloc(bytes + length);
	if (!memory) return -ENOMEM;
	if (length) memcpy(memory + bytes, config, length);
	k_spinlock_key_t key = k_spin_lock(&lock);
	state.spec = *spec;
	state.config_size = length;
	records = memory;
	k_spin_unlock(&lock, key);
	return 0;
}

int application_record_write(const void *data, size_t length)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = !records ? -ENODATA : !state.active ?
		(state.reason == RECORD_TRIGGER_END ? -ENOSPC : -ENODATA) :
		length != state.spec.size ? -EMSGSIZE :
		state.next == UINT64_MAX ? -EOVERFLOW : 0;
	if (!err) {
		memcpy(records + (state.next % state.spec.capacity) * state.spec.size, data, length);
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
	void *data, size_t capacity, struct application_record_info *info)
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
		}
	}
	k_spin_unlock(&lock, key);
	return length;
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
			memcpy(data, records + state.spec.size * state.spec.capacity + offset, length);
		}
	}
	k_spin_unlock(&lock, key);
	return length;
}
