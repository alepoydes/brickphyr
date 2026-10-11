// SPDX-License-Identifier: Apache-2.0
/* Synthetic DMA sink: production service, waveform and Shell, no physical DAC. */
#include "hub_sound.h"
#include "sound_output.h"
#include <string.h>
#include <zephyr/irq_offload.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

static uint16_t *dma_buffer;
static size_t dma_count;
static sound_output_callback callback;
static void *callback_context;
static bool active, hold_start;
static int start_error, stop_error;
static unsigned int next_half, starts, stops;
static K_SEM_DEFINE(started, 0, 1);
static K_SEM_DEFINE(resume, 0, 1);

static int start(const struct device *dev, uint16_t *samples, size_t count,
	uint32_t rate, sound_output_callback cb, void *context, uint32_t *actual)
{
	zassert_false(active);
	dma_buffer = samples;
	dma_count = count;
	callback = cb;
	callback_context = context;
	active = true;
	next_half = 0;
	starts++;
	*actual = rate;
	if (hold_start) {
		hold_start = false;
		k_sem_give(&started);
		zassert_ok(k_sem_take(&resume, K_MSEC(100)));
	}
	return start_error; /* Can fail after taking the buffer: stop must join it. */
}

static int stop(const struct device *dev)
{
	stops++;
	if (!stop_error) {
		active = false;
		callback = NULL;
	}
	return stop_error;
}

static const struct sound_output_api api = {.start = start, .stop = stop};
DEVICE_DT_DEFINE(DT_NODELABEL(hub_speaker), NULL, NULL, NULL, NULL, POST_KERNEL, 90, &api);

static struct bp_sound_state snapshot(void)
{
	struct bp_sound_state value;
	zassert_ok(hub_sound_get(&value));
	return value;
}

static struct bp_sound_state wait_phase(enum bp_sound_phase phase)
{
	int64_t deadline = k_uptime_get() + 100;
	struct bp_sound_state value;
	do {
		k_usleep(100);
		value = snapshot();
		zassert_true(k_uptime_get() < deadline, "phase %u, error %d, cleanup %d",
			value.phase, value.error, value.cleanup_error);
	} while (value.phase != phase);
	return value;
}

static void interrupt(const void *argument)
{
	callback(callback_context, (intptr_t)argument);
}

static void advance(void)
{
	zassert_true(active);
	irq_offload(interrupt, (void *)(intptr_t)next_half);
	next_half ^= 1;
}

static void tone_checks(void)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(shell_execute_cmd(shell, "sound tone 1000 20 100"));
	(void)wait_phase(BP_SOUND_PLAYING);
	zassert_equal(dma_count, 512);
	zassert_equal(hub_sound_tone(BP_SOUND_APPLICATION, 440, 1, 10), -EBUSY);
	zassert_equal(hub_sound_stop(BP_SOUND_APPLICATION), -EACCES);
	/* 1000 Hz at 48000 samples/s: independent period and full-scale levels. */
	for (size_t i = 0; i < 24; i++) {
		zassert_equal(dma_buffer[i], 0);
		zassert_equal(dma_buffer[i + 24], 0xfff0);
		/* The low interval crosses the initial 256-sample half boundary. */
		zassert_equal(dma_buffer[i + 240], 0);
		zassert_equal(dma_buffer[i + 264], 0xfff0);
	}
	advance();
	/* Sample 512 is 32 samples into the next 48-sample period. */
	for (size_t i = 0; i < 16; i++) zassert_equal(dma_buffer[i], 0xfff0);
	for (size_t i = 16; i < 40; i++) zassert_equal(dma_buffer[i], 0);
	for (unsigned int i = 0; i < 4; i++) {
		advance();
	}
	struct bp_sound_state value = wait_phase(BP_SOUND_IDLE);
	zassert_equal(value.completed_samples, 960);
	zassert_ok(value.error);
	zassert_equal(value.owner, BP_SOUND_FREE);
	zassert_false(active);
	zassert_equal(hub_sound_tone(BP_SOUND_APPLICATION, 20001, 0, 10), -EINVAL);
	zassert_equal(hub_sound_tone(BP_SOUND_APPLICATION, 440, 0, 101), -EINVAL);
	zassert_equal(hub_sound_tone(BP_SOUND_FREE, 440, 0, 10), -EINVAL);

	zassert_ok(hub_sound_tone(BP_SOUND_APPLICATION, 440, 0, 10));
	(void)wait_phase(BP_SOUND_PLAYING);
	hub_sound_session_closed();
	zassert_equal(snapshot().owner, BP_SOUND_APPLICATION);
	zassert_ok(hub_sound_release(BP_SOUND_APPLICATION));
	zassert_false(active);
}

static void stream_checks(void)
{
	zassert_equal(hub_sound_stream(BP_SOUND_APPLICATION, 7999), -EINVAL);
	zassert_ok(hub_sound_stream(BP_SOUND_APPLICATION, 16000));
	int16_t chunk[BP_SOUND_CHUNK_SAMPLES];
	for (size_t i = 0; i < ARRAY_SIZE(chunk); i++) {
		chunk[i] = i * 256 - 32768;
	}
	zassert_ok(hub_sound_write(BP_SOUND_APPLICATION, chunk, ARRAY_SIZE(chunk)));
	zassert_equal(snapshot().phase, BP_SOUND_PRIMING);
	zassert_false(active, "Stream started before being primed");
	zassert_equal(hub_sound_write(BP_SOUND_TERMINAL, chunk, 1), -EACCES);
	zassert_equal(hub_sound_write(BP_SOUND_APPLICATION, chunk, 0), -EINVAL);
	zassert_equal(hub_sound_write(BP_SOUND_APPLICATION, chunk, 257), -EINVAL);
	zassert_ok(hub_sound_write(BP_SOUND_APPLICATION, chunk, ARRAY_SIZE(chunk)));
	(void)wait_phase(BP_SOUND_PLAYING);
	for (size_t i = 0; i < dma_count; i++) {
		zassert_equal(dma_buffer[i], (i % ARRAY_SIZE(chunk)) * 256);
	}
	memset(chunk, 0, sizeof(chunk)); /* Original caller memory is no longer used. */
	for (size_t i = 0; i < 4; i++) {
		zassert_ok(hub_sound_write(BP_SOUND_APPLICATION, chunk, ARRAY_SIZE(chunk)));
	}
	zassert_equal(hub_sound_write(BP_SOUND_APPLICATION, chunk, 1), -EAGAIN);
	zassert_equal(snapshot().queued_samples, BP_SOUND_QUEUE_SAMPLES);
	zassert_ok(hub_sound_finish(BP_SOUND_APPLICATION));
	zassert_equal(hub_sound_write(BP_SOUND_APPLICATION, chunk, 1), -EINVAL);
	for (size_t i = 0; i < 7; i++) {
		advance();
	}
	struct bp_sound_state value = wait_phase(BP_SOUND_IDLE);
	zassert_equal(value.completed_samples, 6 * ARRAY_SIZE(chunk));
	zassert_ok(value.error);

	/* Partial final chunks drain once, with silence padding; no stale replay. */
	zassert_ok(hub_sound_stream(BP_SOUND_APPLICATION, 8000));
	const int16_t limits[] = {INT16_MIN, 0, INT16_MAX};
	zassert_ok(hub_sound_write(BP_SOUND_APPLICATION, limits, ARRAY_SIZE(limits)));
	zassert_ok(hub_sound_finish(BP_SOUND_APPLICATION));
	(void)wait_phase(BP_SOUND_PLAYING);
	zassert_equal(dma_buffer[0], 0);
	zassert_equal(dma_buffer[1], 0x8000);
	zassert_equal(dma_buffer[2], 0xfff0);
	for (size_t i = 3; i < dma_count; i++) {
		zassert_equal(dma_buffer[i], 0x8000);
	}
	advance();
	advance();
	value = wait_phase(BP_SOUND_IDLE);
	zassert_equal(value.completed_samples, 3);
	zassert_ok(value.error);

	zassert_ok(hub_sound_stream(BP_SOUND_APPLICATION, 48000));
	zassert_ok(hub_sound_finish(BP_SOUND_APPLICATION));
	value = wait_phase(BP_SOUND_IDLE);
	zassert_equal(value.completed_samples, 0);
	zassert_false(active);
}

static void failure_checks(void)
{
	int16_t chunk[BP_SOUND_CHUNK_SAMPLES] = {0};
	zassert_ok(hub_sound_stream(BP_SOUND_APPLICATION, 48000));
	zassert_ok(hub_sound_write(BP_SOUND_APPLICATION, chunk, ARRAY_SIZE(chunk)));
	zassert_ok(hub_sound_write(BP_SOUND_APPLICATION, chunk, ARRAY_SIZE(chunk)));
	(void)wait_phase(BP_SOUND_PLAYING);
	advance();
	struct bp_sound_state value = wait_phase(BP_SOUND_IDLE);
	zassert_equal(value.error, -EPIPE);
	zassert_equal(value.underruns, 1);
	zassert_equal(value.completed_samples, BP_SOUND_CHUNK_SAMPLES);
	zassert_false(active);

	start_error = -EIO;
	stop_error = -EACCES;
	zassert_ok(hub_sound_tone(BP_SOUND_APPLICATION, 440, 1, 10));
	value = wait_phase(BP_SOUND_FAILED);
	zassert_equal(value.error, -EIO);
	zassert_equal(value.cleanup_error, -EACCES);
	zassert_equal(value.owner, BP_SOUND_APPLICATION);
	zassert_equal(hub_sound_stream(BP_SOUND_TERMINAL, 8000), -EBUSY);
	start_error = stop_error = 0;
	zassert_ok(hub_sound_release(BP_SOUND_APPLICATION));
	value = snapshot();
	zassert_equal(value.error, -EIO, "Cleanup hid the first error");
	zassert_equal(value.phase, BP_SOUND_IDLE);
	zassert_false(active);

	hold_start = true;
	zassert_ok(hub_sound_tone(BP_SOUND_TERMINAL, 440, 0, 10));
	zassert_ok(k_sem_take(&started, K_MSEC(100)));
	hub_sound_session_closed();
	zassert_equal(snapshot().phase, BP_SOUND_STOPPING);
	k_sem_give(&resume);
	(void)wait_phase(BP_SOUND_IDLE);
	zassert_false(active, "Disconnect during start left sound running");

	zassert_ok(hub_sound_tone(BP_SOUND_APPLICATION, 440, 0, 10));
	(void)wait_phase(BP_SOUND_PLAYING);
	irq_offload(interrupt, (void *)(intptr_t)-EIO);
	value = wait_phase(BP_SOUND_IDLE);
	zassert_equal(value.error, -EIO);
	zassert_false(active);

	zassert_ok(hub_sound_tone(BP_SOUND_APPLICATION, 440, 0, 10));
	(void)wait_phase(BP_SOUND_PLAYING);
	zassert_ok(hub_sound_shutdown());
	zassert_equal(hub_sound_tone(BP_SOUND_TERMINAL, 440, 1, 10), -ESHUTDOWN);
	zassert_false(active);
}

ZTEST(sound, test_playback)
{
	zassert_equal(snapshot().phase, BP_SOUND_IDLE);
	zassert_equal(starts, 0);
	tone_checks();
	stream_checks();
	failure_checks();
	zassert_true(stops >= starts);
}
ZTEST_SUITE(sound, NULL, NULL, NULL, NULL, NULL);
