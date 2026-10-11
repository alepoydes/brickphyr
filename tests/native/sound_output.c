// SPDX-License-Identifier: Apache-2.0
/* Explicitly synthetic DMA consumption on the ARM MPU model: one half per
 * scheduler tick. Checks userspace lifetime, not DAC registers or audio timing. */
#include "sound_output.h"
#include <zephyr/kernel.h>

static sound_output_callback callback;
static void *context;
static unsigned int half;
bool sound_output_active;

static void consume(struct k_timer *timer)
{
	if (sound_output_active) {
		callback(context, half);
		half ^= 1;
	}
}
static K_TIMER_DEFINE(timer, consume, NULL);

static int start(const struct device *dev, uint16_t *samples, size_t count,
	uint32_t rate, sound_output_callback cb, void *arg, uint32_t *actual)
{
	callback = cb;
	context = arg;
	half = 0;
	sound_output_active = true;
	*actual = rate;
	k_timer_start(&timer, K_TICKS(1), K_TICKS(1));
	return 0;
}

static int stop(const struct device *dev)
{
	k_timer_stop(&timer);
	sound_output_active = false;
	return 0;
}
static const struct sound_output_api api = {.start = start, .stop = stop};
DEVICE_DT_DEFINE(DT_NODELABEL(hub_speaker), NULL, NULL, NULL, NULL, POST_KERNEL, 90, &api);
