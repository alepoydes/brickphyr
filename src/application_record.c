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
	state.active = false;
	k_spin_unlock(&lock, key);
}

int application_record_begin(const struct bp_record_spec *spec)
{
	if (!spec->size || spec->size > 1024 || !spec->capacity ||
	    spec->capacity > 32768 / spec->size || !spec->schema[0] ||
	    !memchr(spec->schema, 0, sizeof(spec->schema))) return -EINVAL;
	for (size_t i = 0; spec->schema[i]; i++) {
		if (spec->schema[i] < 32 || spec->schema[i] > 126) return -EINVAL;
	}
	/* The supervisor owns configuration and the caller is blocked. Allocate
	 * before publication; no allocator or transport runs inside the data lock. */
	if (records) return -EALREADY;
	uint8_t *memory = k_malloc(spec->size * spec->capacity);
	if (!memory) return -ENOMEM;
	k_spinlock_key_t key = k_spin_lock(&lock);
	state.spec = *spec;
	records = memory;
	k_spin_unlock(&lock, key);
	return 0;
}

int application_record_write(const void *data, size_t length)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = !records || !state.active ? -ENODATA :
		length != state.spec.size ? -EMSGSIZE :
		state.next == UINT64_MAX ? -EOVERFLOW : 0;
	if (!err) {
		memcpy(records + (state.next % state.spec.capacity) * state.spec.size, data, length);
		state.next++;
		if (state.next - state.first > state.spec.capacity) state.first++;
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
