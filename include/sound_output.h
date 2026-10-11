// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>

/* Private device boundary. One privileged service serializes start/stop.
 * A circular buffer contains two equal halves of unsigned, left-aligned
 * 12-bit DAC samples (0x8000 is silence). The buffer outlives stop, including
 * a failed stop. Successful stop quiesces DMA and callbacks before returning.
 * The IRQ callback receives a consumed half (0/1), or a negative error. It may
 * refill that half synchronously; it must not block or call start/stop.
 */
typedef void (*sound_output_callback)(void *context, int half_or_error);

struct sound_output_api {
	int (*start)(const struct device *dev, uint16_t *samples, size_t count,
		uint32_t rate, sound_output_callback callback, void *context, uint32_t *actual_rate);
	int (*stop)(const struct device *dev);
};

static inline int sound_output_start(const struct device *dev, uint16_t *samples,
	size_t count, uint32_t rate, sound_output_callback callback, void *context,
	uint32_t *actual_rate)
{
	const struct sound_output_api *api = dev->api;
	return api->start(dev, samples, count, rate, callback, context, actual_rate);
}

static inline int sound_output_stop(const struct device *dev)
{
	const struct sound_output_api *api = dev->api;
	return api->stop(dev);
}
