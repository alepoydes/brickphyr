// SPDX-License-Identifier: Apache-2.0

#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include "hub_light.h"
#include "led_tlc5955.h"

LOG_MODULE_REGISTER(hub_light);

static const struct device *const leds = DEVICE_DT_GET(DT_NODELABEL(hub_leds));
static K_MUTEX_DEFINE(lock);
static enum { BREATHING, MANUAL, OFF } mode = BREATHING;
static int64_t started = -1;
static int64_t next_frame;

BUILD_ASSERT(DT_NODE_CHILD_IDX(DT_NODELABEL(status_bottom)) ==
	     DT_NODE_CHILD_IDX(DT_NODELABEL(status_top)) + 1);

static int center_color(const uint16_t rgb[3])
{
	const uint16_t pair[] = {rgb[0], rgb[1], rgb[2], rgb[0], rgb[1], rgb[2]};

	return tlc5955_set_colors16(leds, DT_NODE_CHILD_IDX(DT_NODELABEL(status_top)), 2, pair);
}

/* Q12 smoothstep: zero slope at both endpoints, with no floating-point runtime. */
static uint32_t ease(uint32_t time, uint32_t duration)
{
	uint32_t x = MIN(time, duration) * 4095U / duration;

	return (uint64_t)x * x * (3 * 4095 - 2 * x) / (4095U * 4095U);
}

int64_t hub_light_update(int64_t now)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (mode == BREATHING && now >= next_frame) {
		if (started < 0) {
			started = now;
		}
		int64_t elapsed = now - started;
		uint32_t phase = MAX(elapsed - 1200, 0) % 6000;
		uint32_t blend = ease(phase <= 3000 ? phase : 6000 - phase, 3000);
		uint32_t fade = ease(MIN(elapsed, 1200), 1200);
		const uint16_t blue[] = {512, 1792, 4095};
		uint16_t rgb[3];

		for (int i = 0; i < 3; i++) {
			uint32_t level = 4095 - ((4095 - blue[i]) * blend + 2047) / 4095;

			level = (level * fade + 2047) / 4095;
			/* Gamma 2 approximation; peak duty is 4095/65535 (6.25%). */
			rgb[i] = (level * level + 2047) / 4095;
		}
		int err = device_is_ready(leds) ? center_color(rgb) : -ENODEV;

		if (err) {
			mode = MANUAL;
			LOG_ERR("Readiness light stopped: %d", err);
		}
		/* 50 frames/s: one 97-byte latch per frame, no catch-up burst after a delay. */
		next_frame = started + (elapsed / 20 + 1) * 20;
	}
	int64_t deadline = mode == BREATHING ? next_frame : INT64_MAX;

	k_mutex_unlock(&lock);
	return deadline;
}

int hub_light_manual(bool center, const uint8_t rgb[3])
{
	k_mutex_lock(&lock, K_FOREVER);
	int err = mode == OFF ? -ESHUTDOWN : !device_is_ready(leds) ? -ENODEV : 0;

	if (!err) {
		if (center) {
			const uint16_t color[] = {rgb[0] * 257U, rgb[1] * 257U, rgb[2] * 257U};

			mode = MANUAL;
			err = center_color(color);
		} else {
			err = led_set_color(leds, DT_NODE_CHILD_IDX(DT_NODELABEL(bluetooth_led)),
					    3, rgb);
		}
	}
	k_mutex_unlock(&lock);
	return err;
}

int hub_light_off(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	mode = OFF;
	int err = -ENODEV;

	if (device_is_ready(leds)) {
		const uint16_t off[] = {0, 0, 0};
		const uint8_t off8[] = {0, 0, 0};

		err = center_color(off);
		int bt_err = led_set_color(leds, DT_NODE_CHILD_IDX(DT_NODELABEL(bluetooth_led)),
					   3, off8);

		if (!err) {
			err = bt_err;
		}
	}
	k_mutex_unlock(&lock);
	return err;
}
