// SPDX-License-Identifier: Apache-2.0

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "hub_light.h"
#include "led_tlc5955.h"

LOG_MODULE_REGISTER(hub_light);

static const struct device *const leds = DEVICE_DT_GET(DT_NODELABEL(hub_leds));
static const uint8_t white_balance[] = DT_PROP(DT_NODELABEL(hub_leds), white_balance);
static K_MUTEX_DEFINE(lock);
static enum { ANIMATED, MANUAL, OFF } mode = ANIMATED;
static int64_t started = -1;
static int64_t next_frame;
static uint32_t palette_from;
static uint32_t palette_target;
static int64_t palette_changed;
/* Animation-local libc PRNG: no entropy wait or shared random state. */
static unsigned int random_seed = 1;
static int64_t next_flare;
static int64_t next_surge;
/* Ripple pulses last at most 180 ms, with onsets at least 60 ms apart.
 * Three slots retain every live pulse; the rare surge has its own envelope.
 */
static struct flare {
	int64_t started;
	uint16_t peak, attack, decay;
} flares[3], surge;
static unsigned int next_slot;

BUILD_ASSERT(DT_NODE_CHILD_IDX(DT_NODELABEL(status_bottom)) ==
	     DT_NODE_CHILD_IDX(DT_NODELABEL(status_top)) + 1);
BUILD_ASSERT(ARRAY_SIZE(white_balance) == 3);
#define CHECK_WEIGHT(node, prop, index) \
	BUILD_ASSERT(DT_PROP_BY_IDX(node, prop, index) >= 0 && \
		     DT_PROP_BY_IDX(node, prop, index) <= 255);
DT_FOREACH_PROP_ELEM(DT_NODELABEL(hub_leds), white_balance, CHECK_WEIGHT)

/* Full-range RGB in, linear PWM out. Apply white balance after gamma, once. */
static void color_to_pwm(uint16_t rgb[3], uint16_t peak)
{
	for (int i = 0; i < 3; i++) {
		uint32_t linear = ((uint32_t)rgb[i] * rgb[i] + 32767) / UINT16_MAX;
		uint32_t balanced = (linear * white_balance[i] + 127) / 255;

		rgb[i] = (balanced * peak + 32767) / UINT16_MAX;
	}
}

static int write_pwm(bool center, const uint16_t duty[3])
{
	const uint16_t pair[] = {duty[0], duty[1], duty[2], duty[0], duty[1], duty[2]};
	uint32_t first = center ? DT_NODE_CHILD_IDX(DT_NODELABEL(status_top)) :
		DT_NODE_CHILD_IDX(DT_NODELABEL(bluetooth_led));

	return tlc5955_set_colors16(leds, first, center ? 2 : 1, pair);
}

/* Q12 smoothstep: zero slope at both endpoints, with no floating-point runtime. */
static uint32_t ease(uint32_t time, uint32_t duration)
{
	uint32_t x = MIN(time, duration) * 4095U / duration;

	return (uint64_t)x * x * (3 * 4095 - 2 * x) / (4095U * 4095U);
}

static uint32_t palette_blend(int64_t now)
{
	uint32_t blend = ease(CLAMP(now - palette_changed, 0, 300), 300);
	return (palette_from * (4095 - blend) + palette_target * blend + 2047) / 4095;
}

/* Q12 additive light gain; a new pulse never changes an existing envelope. */
static uint32_t flare_level(const struct flare *flare, int64_t now)
{
	if (!flare->peak) {
		return 0;
	}
	int64_t age = now - flare->started;

	if (age >= flare->attack + flare->decay) {
		return 0;
	}
	if (age < flare->attack) {
		return flare->peak * ease(age, flare->attack) / 4095;
	}
	return flare->peak * (4095 - ease(age - flare->attack, flare->decay)) / 4095;
}

static uint32_t reactor_gain(int64_t now, uint32_t palette)
{
	if (now >= next_flare) {
		struct flare *flare = &flares[next_slot];

		*flare = (struct flare){
			.started = now,
			.peak = 82 + rand_r(&random_seed) % 124,
			.attack = 60 + 20 * (rand_r(&random_seed) % 2),
			.decay = 80 + 20 * (rand_r(&random_seed) % 2),
		};
		next_slot = (next_slot + 1) % ARRAY_SIZE(flares);
		/* Independent onsets; a delayed frame never replays missed pulses. */
		next_flare = now + 60 + 20 * (rand_r(&random_seed) % 3);
	}
	if (now >= next_surge) {
		surge = (struct flare){
			.started = now,
			.peak = 410 + rand_r(&random_seed) % 410,
			.attack = 80 + 20 * (rand_r(&random_seed) % 3),
			.decay = 60 + 20 * (rand_r(&random_seed) % 3),
		};
		next_surge = now + 2500 + rand_r(&random_seed) % 2501;
	}
	/* Cyan base 40%, yellow 95%; ripple adds 2..5%, rare surges 10..20%. */
	uint32_t gain = 1638 + (3890 - 1638) * palette / 4095 + flare_level(&surge, now);

	for (int i = 0; i < ARRAY_SIZE(flares); i++) {
		gain += flare_level(&flares[i], now);
	}
	return gain;
}

int hub_light_select(enum host_mode selected, bool menu, int64_t now)
{
	if (selected != HOST_PYBRICKS && selected != HOST_SHELL) {
		return -EINVAL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	int err = mode == OFF ? -ESHUTDOWN : !device_is_ready(leds) ? -ENODEV : 0;
	if (!err) {
		uint16_t white[] = {UINT16_MAX, UINT16_MAX, UINT16_MAX};

		color_to_pwm(white, menu ? UINT16_MAX : 0);
		err = write_pwm(false, white);
		if (!err) {
			palette_from = palette_blend(now);
			palette_target = selected == HOST_SHELL ? 4095 : 0;
			palette_changed = now;
			mode = ANIMATED;
		}
	}
	k_mutex_unlock(&lock);
	return err;
}

int64_t hub_light_update(int64_t now)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (mode == ANIMATED && now >= next_frame) {
		if (started < 0) {
			started = now;
			next_flare = now + 1500;
			next_surge = now + 3500;
		}
		int64_t elapsed = now - started;
		uint32_t fade = ease(MIN(elapsed, 1200), 1200);
		uint32_t palette = palette_blend(now);
		/* Saturated cyan/yellow. Base hue changes when selecting an interface. */
		uint16_t rgb[] = {UINT16_MAX * palette / 4095, UINT16_MAX,
			UINT16_MAX * (4095 - palette) / 4095};

		color_to_pwm(rgb, UINT16_MAX);
		uint32_t maximum = MAX(rgb[0], MAX(rgb[1], rgb[2]));
		uint32_t gain = reactor_gain(now, palette) * fade / 4095;

		for (int i = 0; i < 3; i++) {
			uint32_t normalized = maximum ? (uint32_t)rgb[i] * UINT16_MAX / maximum : 0;

			/* Brightness can exceed unity: saturation itself shifts the hue. */
			rgb[i] = MIN(normalized * gain / 4095, UINT16_MAX);
		}
		int err = device_is_ready(leds) ? write_pwm(true, rgb) : -ENODEV;

		if (err) {
			mode = MANUAL;
			LOG_ERR("Readiness light stopped: %d", err);
		}
		/* 50 frames/s: one 97-byte latch per frame, no catch-up burst after a delay. */
		next_frame = started + (elapsed / 20 + 1) * 20;
	}
	int64_t deadline = mode == ANIMATED ? next_frame : INT64_MAX;

	k_mutex_unlock(&lock);
	return deadline;
}

int hub_light_manual(bool center, const uint8_t rgb[3])
{
	uint16_t duty[] = {rgb[0] * 257U, rgb[1] * 257U, rgb[2] * 257U};

	color_to_pwm(duty, UINT16_MAX);
	return hub_light_pwm(center, duty);
}

int hub_light_pwm(bool center, const uint16_t duty[3])
{
	k_mutex_lock(&lock, K_FOREVER);
	int err = mode == OFF ? -ESHUTDOWN : !device_is_ready(leds) ? -ENODEV : 0;

	if (!err) {
		if (center) {
			mode = MANUAL;
		}
		err = write_pwm(center, duty);
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

		err = write_pwm(true, off);
		int bt_err = write_pwm(false, off);

		if (!err) {
			err = bt_err;
		}
	}
	k_mutex_unlock(&lock);
	return err;
}
