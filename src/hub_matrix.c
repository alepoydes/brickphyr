// SPDX-License-Identifier: Apache-2.0

#define __ZEPHYR_SUPERVISOR__
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include "hub_matrix.h"
#include "led_tlc5955.h"

#define MATRIX DT_NODELABEL(hub_matrix)
static const struct device *const leds = DEVICE_DT_GET(DT_PHANDLE(MATRIX, led_controller));
static const uint8_t channels[] = DT_PROP(MATRIX, channels);
BUILD_ASSERT(ARRAY_SIZE(channels) == BP_MATRIX_PIXELS);

static struct k_spinlock lock;
static struct bp_matrix_state state;
static bool releasing;
static bool disabled;
static uint32_t revision;
static uint8_t application_rotation;
static uint8_t *animation;
static size_t animation_count;
static uint32_t interval;
static int64_t epoch;

static void apply(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(update, apply);

/* All helpers below run under lock. No application memory remains borrowed. */
static void stop_animation(void)
{
	k_free(animation);
	animation = NULL;
	state.animating = false;
}

static void copy_pixels(uint32_t first, const uint8_t *pixels, size_t count,
			uint8_t rotation)
{
	for (size_t i = 0; i < count; i++) {
		size_t row = (first + i) / 5, col = (first + i) % 5;
		size_t index;
		switch (rotation) {
		case 1: index = col * 5 + 4 - row; break;
		case 2: index = (4 - row) * 5 + 4 - col; break;
		case 3: index = (4 - col) * 5 + row; break;
		default: index = first + i; break;
		}
		state.requested[index] = pixels[i];
	}
}

static int admission(enum bp_matrix_owner owner)
{
	return disabled ? -ESHUTDOWN : releasing || state.pending ||
		(state.owner != BP_MATRIX_FREE && state.owner != owner) ? -EBUSY : 0;
}

static void apply(struct k_work *work)
{
	uint8_t frame[BP_MATRIX_PIXELS];
	uint16_t pwm[BP_MATRIX_PIXELS];
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (!state.pending && !state.animating) {
		k_spin_unlock(&lock, key);
		return;
	}
	if (state.animating) {
		int64_t now = k_uptime_get();
		if (epoch < 0) {
			epoch = now;
		}
		size_t index = ((uint64_t)(now - epoch) / interval) % animation_count;
		copy_pixels(0, animation + index * BP_MATRIX_PIXELS, BP_MATRIX_PIXELS,
			application_rotation);
	}
	state.pending = true;
	memcpy(frame, state.requested, sizeof(frame));
	uint32_t current = revision;
	k_spin_unlock(&lock, key);
	for (size_t i = 0; i < ARRAY_SIZE(pwm); i++) {
		/* Pybricks LED-array brightness semantics: percentage with gamma 2. */
		pwm[i] = UINT16_MAX * (uint32_t)frame[i] * frame[i] / 10000;
	}
	int err = tlc5955_set_channels16(leds, channels, pwm, ARRAY_SIZE(pwm));
	key = k_spin_lock(&lock);
	state.error = err;
	state.valid = !err;
	if (err) {
		state.errors += state.errors != UINT32_MAX;
	} else {
		memcpy(state.applied, frame, sizeof(frame));
		state.applied_ms = k_uptime_get();
	}
	/* Closing an owner can replace an in-flight frame with the final blank.
	 * That submission is already queued; never release before it completes. */
	if (current == revision) {
		state.pending = false;
		if (err) {
			stop_animation();
		} else if (releasing) {
			state.owner = BP_MATRIX_FREE;
			releasing = false;
		} else if (state.animating) {
			/* Skip missed frames without accumulating driver/queue latency. */
			int64_t next = epoch + ((state.applied_ms - epoch) / interval + 1) * interval;
			k_work_reschedule(&update, K_TIMEOUT_ABS_MS(next));
		}
	}
	k_spin_unlock(&lock, key);
}

int hub_matrix_write(enum bp_matrix_owner owner, uint32_t first, const uint8_t *values,
		     size_t count)
{
	if ((owner != BP_MATRIX_APPLICATION && owner != BP_MATRIX_TERMINAL) || !values ||
	    !count || first >= BP_MATRIX_PIXELS || count > BP_MATRIX_PIXELS - first) {
		return -EINVAL;
	}
	for (size_t i = 0; i < count; i++) {
		if (values[i] > 100) {
			return -EINVAL;
		}
	}
	if (!device_is_ready(leds)) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = admission(owner);
	if (!err) {
		bool animated = state.animating;
		/* Rescheduling and publication are atomic with respect to the worker. */
		err = k_work_reschedule(&update, K_NO_WAIT);
		if (err >= 0) {
			k_free(animation);
			animation = NULL;
			state.animating = false;
			if (animated) {
				memset(state.requested, 0, sizeof(state.requested));
			}
			copy_pixels(first, values, count,
				owner == BP_MATRIX_APPLICATION ? application_rotation : 0);
			state.owner = owner;
			state.pending = true;
			revision++;
			err = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_matrix_animate(const uint8_t *frames, size_t count, uint32_t interval_ms)
{
	if (!frames || !count || count > BP_MATRIX_MAX_FRAMES || !interval_ms ||
	    interval_ms > INT32_MAX) {
		return -EINVAL;
	}
	size_t bytes = count * BP_MATRIX_PIXELS;
	for (size_t i = 0; i < bytes; i++) {
		if (frames[i] > 100) {
			return -EINVAL;
		}
	}
	if (!device_is_ready(leds)) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = admission(BP_MATRIX_APPLICATION);
	if (!err) {
		/* Nonwaiting allocation and <=6375-byte publication under the lock
		 * prevent a forced application stop from leaking an unpublished copy. */
		uint8_t *copy = k_malloc(bytes);
		if (!copy) {
			err = -ENOMEM;
		} else {
			err = k_work_reschedule(&update, K_NO_WAIT);
			if (err >= 0) {
				memcpy(copy, frames, bytes);
				k_free(animation);
				animation = copy;
				animation_count = count;
				interval = interval_ms;
				epoch = -1;
				state.animating = true;
				state.pending = true;
				state.owner = BP_MATRIX_APPLICATION;
				revision++;
				err = 0;
			} else {
				k_free(copy);
			}
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_matrix_orientation(uint32_t rotation)
{
	if (rotation > 3) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	application_rotation = rotation;
	k_spin_unlock(&lock, key);
	return 0;
}

int hub_matrix_get(struct bp_matrix_state *value)
{
	if (!value) {
		return -EINVAL;
	}
	if (!device_is_ready(leds)) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	*value = state;
	k_spin_unlock(&lock, key);
	return 0;
}

/* Caller holds lock. A failed blank retains ownership and can be retried. */
static int blank(void)
{
	stop_animation();
	int err = k_work_reschedule(&update, K_NO_WAIT);
	if (err < 0) {
		state.error = err;
		return err;
	}
	memset(state.requested, 0, sizeof(state.requested));
	state.pending = true;
	releasing = true;
	revision++;
	return 0;
}

int hub_matrix_wait(void)
{
	struct k_work_sync sync;
	/* Only submitted writes count as pending, not future animation deadlines.
	 * A disconnect can submit a final blank while an earlier frame runs. */
	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool pending = state.pending;
		int err = state.error;
		k_spin_unlock(&lock, key);
		if (!pending) {
			return err;
		}
		k_work_flush_delayable(&update, &sync);
	}
}

int hub_matrix_release(enum bp_matrix_owner owner)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (owner == BP_MATRIX_APPLICATION) {
		application_rotation = 0;
	}
	bool owned = state.owner == owner && owner != BP_MATRIX_FREE;
	int err = owned ? blank() : 0;
	k_spin_unlock(&lock, key);
	if (!owned || err) {
		return err;
	}
	struct k_work_sync sync;
	for (;;) {
		k_work_flush_delayable(&update, &sync);
		key = k_spin_lock(&lock);
		owned = state.owner == owner;
		bool pending = owned && state.pending;
		err = owned ? state.error : 0;
		k_spin_unlock(&lock, key);
		if (!pending) {
			return err;
		}
	}
}

void hub_matrix_session_closed(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (state.owner == BP_MATRIX_TERMINAL) {
		(void)blank();
	}
	k_spin_unlock(&lock, key);
}

int hub_matrix_shutdown(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	disabled = true;
	int err = device_is_ready(leds) ? blank() : -ENODEV;
	k_spin_unlock(&lock, key);
	return err ? err : hub_matrix_wait();
}

#ifdef CONFIG_SHELL
static int cmd_matrix(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "clear")) {
		if (!device_is_ready(leds)) {
			return -ENODEV;
		}
		k_spinlock_key_t key = k_spin_lock(&lock);
		int err = disabled ? -ESHUTDOWN : state.owner == BP_MATRIX_APPLICATION ?
			-EBUSY : blank();
		k_spin_unlock(&lock, key);
		return err ? err : hub_matrix_wait();
	}
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) {
		struct bp_matrix_state value;
		int err = hub_matrix_get(&value);
		if (err) {
			return err;
		}
		shell_print(shell, "Owner %u; pending %u; valid %u; error %d (%u failures)",
			value.owner, value.pending, value.valid, value.error, value.errors);
		for (size_t i = 0; i < BP_MATRIX_PIXELS; i += 5) {
			shell_print(shell, "%3u %3u %3u %3u %3u", value.applied[i], value.applied[i + 1],
				value.applied[i + 2], value.applied[i + 3], value.applied[i + 4]);
		}
		return 0;
	}
	uint8_t values[BP_MATRIX_PIXELS];
	uint32_t first = 0;
	size_t count = BP_MATRIX_PIXELS;
	int err = 0;
	if (argc == 5 && !strcmp(argv[1], "pixel")) {
		unsigned long row = shell_strtoul(argv[2], 10, &err);
		unsigned long col = shell_strtoul(argv[3], 10, &err);
		unsigned long level = shell_strtoul(argv[4], 10, &err);
		if (err || row >= 5 || col >= 5 || level > 100) {
			return -EINVAL;
		}
		first = row * 5 + col;
		count = 1;
		values[0] = level;
	} else if (argc == 3 && !strcmp(argv[1], "fill")) {
		unsigned long level = shell_strtoul(argv[2], 10, &err);
		if (err || level > 100) {
			return -EINVAL;
		}
		memset(values, level, sizeof(values));
	} else if (argc == BP_MATRIX_PIXELS + 2 && !strcmp(argv[1], "frame")) {
		for (size_t i = 0; i < count; i++) {
			unsigned long level = shell_strtoul(argv[i + 2], 10, &err);
			if (err || level > 100) {
				return -EINVAL;
			}
			values[i] = level;
		}
	} else {
		shell_error(shell, "matrix [status | clear | fill 0..100 | pixel row col 0..100 | frame <25 levels>]");
		return -EINVAL;
	}
	err = hub_matrix_write(BP_MATRIX_TERMINAL, first, values, count);
	return err ? err : hub_matrix_wait();
}

SHELL_CMD_ARG_REGISTER(matrix, NULL, "5x5 matrix; row/column 0..4, brightness 0..100.",
		      cmd_matrix, 1, BP_MATRIX_PIXELS + 1);
#endif
