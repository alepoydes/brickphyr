// SPDX-License-Identifier: Apache-2.0
#include "hub_sound.h"
#include "sound_output.h"
#include "sound/waveform.h"

#include <inttypes.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#define HALF BP_SOUND_CHUNK_SAMPLES
#define TONE_RATE 48000
static const struct device *const output = DEVICE_DT_GET(DT_NODELABEL(hub_speaker));
static struct k_spinlock lock;
static struct bp_sound_state state;
static uint16_t buffer[2 * HALF], queue[BP_SOUND_QUEUE_SAMPLES];
static size_t read_index, half_valid[2];
static uint64_t remaining;
static struct sound_waveform tone;
static bool disabled, reserved;

static void update(struct k_work *work);
static K_WORK_DEFINE(work, update);

/* All helpers below run under lock. One ISR copies/fills at most one half;
 * applications can only change firmware-owned queue data, never DMA memory. */
static int submit(void)
{
	int err = k_work_submit(&work);
	if (err < 0) {
		state.cleanup_error = err;
		state.phase = BP_SOUND_FAILED;
	}
	return err < 0 ? err : 0;
}

static int stopping(int error)
{
	if (error && !state.error) {
		state.error = error;
		state.failures += state.failures != UINT32_MAX;
		state.underruns += error == -EPIPE && state.underruns != UINT32_MAX;
	}
	state.phase = BP_SOUND_STOPPING;
	state.changed_ms = k_uptime_get();
	return submit();
}

static int fill(unsigned int half)
{
	uint16_t *dest = buffer + half * HALF;
	size_t count;
	if (state.mode == BP_SOUND_TONE) {
		count = MIN(remaining, HALF);
		sound_waveform_fill(&tone, dest, count);
		if (state.duration_ms) {
			remaining -= count;
		}
	} else {
		count = MIN(state.queued_samples, HALF);
		if (count < HALF && !state.eof) {
			return -EPIPE;
		}
		for (size_t i = 0; i < count; i++) {
			dest[i] = queue[read_index];
			read_index = (read_index + 1) % ARRAY_SIZE(queue);
		}
		state.queued_samples -= count;
	}
	for (size_t i = count; i < HALF; i++) {
		dest[i] = 0x8000;
	}
	half_valid[half] = count;
	return 0;
}

static void consumed(void *context, int half)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (half < 0) {
		stopping(half);
	} else if (state.phase == BP_SOUND_PLAYING) {
		bool had_data = half_valid[half] != 0;
		state.completed_samples += half_valid[half];
		half_valid[half] = 0;
		int err = fill(half);
		/* Drain a silent half after the last payload. DMA completion precedes
		 * the DAC's last conversion by one trigger. */
		if (err || (!had_data && !half_valid[0] && !half_valid[1])) {
			stopping(err);
		}
	}
	/* A stop request or queue underrun must not replay an old half while
	 * the worker is joining DMA. The other half is still firmware-owned. */
	if (half >= 0 && state.phase != BP_SOUND_PLAYING) {
		for (size_t i = half * HALF; i < (half + 1) * HALF; i++) {
			buffer[i] = 0x8000;
		}
	}
	k_spin_unlock(&lock, key);
}

static void update(struct k_work *item)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (state.phase == BP_SOUND_STOPPING) {
		k_spin_unlock(&lock, key);
		int err = sound_output_stop(output);
		key = k_spin_lock(&lock);
		state.cleanup_error = err;
		if (err) {
			if (!state.error) {
				state.error = err;
				state.failures += state.failures != UINT32_MAX;
			}
			state.phase = BP_SOUND_FAILED;
		} else {
			state.phase = BP_SOUND_IDLE;
			if (!reserved) {
				state.owner = BP_SOUND_FREE;
			}
			state.queued_samples = 0;
		}
		state.changed_ms = k_uptime_get();
		k_spin_unlock(&lock, key);
		return;
	}
	if (state.phase != BP_SOUND_PRIMING || (state.mode == BP_SOUND_PCM &&
	    state.queued_samples < 2 * HALF && !state.eof)) {
		k_spin_unlock(&lock, key);
		return;
	}
	if (state.mode == BP_SOUND_TONE) {
		/* Volume conversion is cold-path work, never in the audio ISR. */
		uint32_t frequency = state.frequency, volume = state.volume;
		k_spin_unlock(&lock, key);
		sound_waveform_init(&tone, frequency, TONE_RATE, volume);
		key = k_spin_lock(&lock);
		if (state.phase != BP_SOUND_PRIMING) {
			k_spin_unlock(&lock, key);
			return;
		}
	}
	(void)fill(0);
	/* Give byte-fed UARTs an IRQ opportunity between the initial halves,
	 * just as they get between DMA refills during playback. */
	k_spin_unlock(&lock, key);
	key = k_spin_lock(&lock);
	if (state.phase != BP_SOUND_PRIMING) {
		k_spin_unlock(&lock, key);
		return;
	}
	(void)fill(1);
	if (!half_valid[0] && !half_valid[1]) {
		stopping(0);
		k_spin_unlock(&lock, key);
		return;
	}
	uint32_t rate = state.requested_rate;
	/* Publish admission before enabling IRQs. Stop can be requested during
	 * device start; the same worker then completes the queued stop. */
	state.phase = BP_SOUND_PLAYING;
	state.changed_ms = k_uptime_get();
	k_spin_unlock(&lock, key);
	uint32_t actual;
	int err = sound_output_start(output, buffer, ARRAY_SIZE(buffer), rate, consumed, NULL, &actual);
	key = k_spin_lock(&lock);
	if (err) {
		stopping(err);
	} else {
		state.actual_rate = actual;
	}
	k_spin_unlock(&lock, key);
}

static int acquire(enum bp_sound_owner owner, enum bp_sound_mode mode, uint32_t rate)
{
	if (owner != BP_SOUND_APPLICATION && owner != BP_SOUND_TERMINAL) {
		return -EINVAL;
	}
	if (!device_is_ready(output)) {
		return -ENODEV;
	}
	if (disabled || (state.owner != BP_SOUND_FREE &&
	    !(reserved && state.owner == owner && state.phase == BP_SOUND_IDLE))) {
		return disabled ? -ESHUTDOWN : -EBUSY;
	}
	state = (struct bp_sound_state){.owner = owner, .phase = BP_SOUND_PRIMING,
		.mode = mode, .requested_rate = rate, .changed_ms = k_uptime_get(),
		.failures = state.failures, .underruns = state.underruns};
	read_index = 0;
	half_valid[0] = half_valid[1] = 0;
	return 0;
}

int hub_sound_reserve(enum bp_sound_owner owner)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = state.owner != BP_SOUND_FREE ? -EBUSY : acquire(owner, BP_SOUND_TONE, TONE_RATE);
	if (!err) {
		reserved = true;
		state.phase = BP_SOUND_IDLE;
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_sound_tone(enum bp_sound_owner owner, uint32_t frequency, uint32_t duration_ms,
	uint32_t volume)
{
	if ((frequency && frequency < 20) || frequency > 24000 || volume > 100) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = acquire(owner, BP_SOUND_TONE, TONE_RATE);
	if (!err) {
		state.frequency = frequency;
		state.duration_ms = duration_ms;
		state.volume = volume;
		remaining = duration_ms ? (uint64_t)TONE_RATE * duration_ms / 1000 : UINT64_MAX;
		err = submit();
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_sound_stream(enum bp_sound_owner owner, uint32_t rate)
{
	if (rate < 8000 || rate > 48000) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = acquire(owner, BP_SOUND_PCM, rate);
	k_spin_unlock(&lock, key);
	return err;
}

static int writable(enum bp_sound_owner owner)
{
	return disabled ? -ESHUTDOWN : state.owner != owner || owner == BP_SOUND_FREE ? -EACCES :
		state.mode != BP_SOUND_PCM || state.eof ||
		(state.phase != BP_SOUND_PRIMING && state.phase != BP_SOUND_PLAYING) ? -EINVAL : 0;
}

int hub_sound_write(enum bp_sound_owner owner, const int16_t *samples, size_t count)
{
	if (!samples || !count || count > BP_SOUND_CHUNK_SAMPLES) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = writable(owner);
	if (!err && count > ARRAY_SIZE(queue) - state.queued_samples) {
		err = -EAGAIN;
	}
	if (!err) {
		size_t index = (read_index + state.queued_samples) % ARRAY_SIZE(queue);
		for (size_t i = 0; i < count; i++) {
			queue[index] = ((uint16_t)samples[i] ^ 0x8000U) & 0xfff0U;
			index = (index + 1) % ARRAY_SIZE(queue);
		}
		state.queued_samples += count;
		err = submit();
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_sound_finish(enum bp_sound_owner owner)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = writable(owner);
	if (!err) {
		state.eof = true;
		err = submit();
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_sound_stop(enum bp_sound_owner owner)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = owner != BP_SOUND_APPLICATION && owner != BP_SOUND_TERMINAL ? -EINVAL :
		state.owner != BP_SOUND_FREE && state.owner != owner ? -EACCES : 0;
	if (!err && state.owner == owner) {
		reserved = false;
		err = stopping(0);
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_sound_get(struct bp_sound_state *value)
{
	if (!value) {
		return -EINVAL;
	}
	if (!device_is_ready(output)) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	*value = state;
	k_spin_unlock(&lock, key);
	return 0;
}

int hub_sound_release(enum bp_sound_owner owner)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool owned = owner != BP_SOUND_FREE && state.owner == owner;
	if (owned) {
		reserved = false;
		stopping(0);
	}
	k_spin_unlock(&lock, key);
	if (!owned) {
		return 0;
	}
	struct k_work_sync sync;
	for (;;) {
		k_work_flush(&work, &sync);
		key = k_spin_lock(&lock);
		bool pending = state.owner == owner && state.phase == BP_SOUND_STOPPING;
		int err = state.owner == owner ? state.cleanup_error : 0;
		k_spin_unlock(&lock, key);
		if (!pending) {
			return err;
		}
	}
}

void hub_sound_session_closed(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (state.owner == BP_SOUND_TERMINAL) {
		reserved = false;
		stopping(0);
	}
	k_spin_unlock(&lock, key);
}

int hub_sound_shutdown(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	disabled = true;
	enum bp_sound_owner owner = state.owner;
	k_spin_unlock(&lock, key);
	return hub_sound_release(owner);
}

#ifdef CONFIG_SHELL
static int cmd_sound(const struct shell *shell, size_t argc, char **argv)
{
	int err;
	if (argc == 2 && !strcmp(argv[1], "stop")) {
		err = hub_sound_stop(BP_SOUND_TERMINAL);
		return err ? err : hub_sound_release(BP_SOUND_TERMINAL);
	}
	if (argc == 5 && !strcmp(argv[1], "tone")) {
		uint32_t args[3];
		for (size_t i = 0; i < 3; i++) {
			err = 0;
			unsigned long value = shell_strtoul(argv[i + 2], 10, &err);
			if (err || argv[i + 2][0] == '-' || value > UINT32_MAX) {
				return -EINVAL;
			}
			args[i] = value;
		}
		return hub_sound_tone(BP_SOUND_TERMINAL, args[0], args[1], args[2]);
	}
	if (argc != 1 && (argc != 2 || strcmp(argv[1], "status"))) {
		shell_error(shell, "Usage: sound [status | tone HZ MS VOLUME | stop]");
		return -EINVAL;
	}
	struct bp_sound_state value;
	err = hub_sound_get(&value);
	if (!err) {
		shell_print(shell, "Owner %u; phase %u; mode %u; rate %u/%u Hz; queued %u; completed %" PRIu64,
			value.owner, value.phase, value.mode, value.actual_rate, value.requested_rate,
			value.queued_samples, value.completed_samples);
		shell_print(shell, "Error %d; cleanup %d; underruns %u; failures %u",
			value.error, value.cleanup_error, value.underruns, value.failures);
	}
	return err;
}
SHELL_CMD_ARG_REGISTER(sound, NULL, "Speaker tone, stop and status.", cmd_sound, 1, 4);
#endif
