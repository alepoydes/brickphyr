// SPDX-License-Identifier: Apache-2.0
/* Explicitly synthetic DMA consumption on the ARM MPU model: one half per
 * scheduler tick. Checks userspace lifetime, not DAC registers or audio timing. */
#include "sound_output.h"
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static sound_output_callback callback;
static void *context;
static unsigned int half;
bool sound_output_active;
static atomic_t start_error, stop_error;

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
	return atomic_get(&start_error) ? -EIO : 0;
}

static int stop(const struct device *dev)
{
	if (atomic_get(&stop_error)) return -EACCES;
	k_timer_stop(&timer);
	sound_output_active = false;
	return 0;
}
static const struct sound_output_api api = {.start = start, .stop = stop};
DEVICE_DT_DEFINE(DT_NODELABEL(hub_speaker), NULL, NULL, NULL, NULL, POST_KERNEL, 90, &api);

#ifdef CONFIG_SHELL
static int failure(const struct shell *shell, size_t argc, char **argv)
{
	int err = 0;
	unsigned long start = shell_strtoul(argv[1], 10, &err);
	if (err || start > 1) return -EINVAL;
	unsigned long stop = shell_strtoul(argv[2], 10, &err);
	if (err || stop > 1) return -EINVAL;
	atomic_set(&start_error, start);
	atomic_set(&stop_error, stop);
	return 0;
}
SHELL_CMD_ARG_REGISTER(sound_error, NULL, "Synthetic DMA start/stop failure: 0|1 0|1.", failure, 3, 0);
#endif
