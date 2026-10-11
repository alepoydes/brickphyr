// SPDX-License-Identifier: Apache-2.0

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
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
static struct palette {
	uint32_t from, target;
	int64_t changed;
} center_palette, bluetooth_palette;
static enum host_ble_status bluetooth_state;
static int64_t bluetooth_changed;
static bool bluetooth_manual;
static enum hub_light_menu menu_state;
/* Center top, center bottom, Bluetooth: one latch also preserves manual colors. */
static uint16_t colors[9];
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
BUILD_ASSERT(DT_NODE_CHILD_IDX(DT_NODELABEL(bluetooth_led)) ==
	     DT_NODE_CHILD_IDX(DT_NODELABEL(status_top)) + 2);
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

static int write_colors(void)
{
	return tlc5955_set_colors16(leds, DT_NODE_CHILD_IDX(DT_NODELABEL(status_top)),
		3, colors);
}

static int write_pwm(bool center, const uint16_t duty[3])
{
	memcpy(colors + (center ? 0 : 6), duty, 3 * sizeof(*duty));
	if (center) {
		memcpy(colors + 3, duty, 3 * sizeof(*duty));
	}
	return write_colors();
}

/* Q12 smoothstep: zero slope at both endpoints, with no floating-point runtime. */
static uint32_t ease(uint32_t time, uint32_t duration)
{
	uint32_t x = MIN(time, duration) * 4095U / duration;

	return (uint64_t)x * x * (3 * 4095 - 2 * x) / (4095U * 4095U);
}

static uint32_t palette_blend(const struct palette *palette, int64_t now)
{
	uint32_t blend = ease(CLAMP(now - palette->changed, 0, 300), 300);
	return (palette->from * (4095 - blend) + palette->target * blend + 2047) / 4095;
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

static uint32_t reactor_fluctuation(int64_t now)
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
	uint32_t gain = flare_level(&surge, now);

	for (int i = 0; i < ARRAY_SIZE(flares); i++) {
		gain += flare_level(&flares[i], now);
	}
	return gain;
}

/* One animation and pulse clock for both buttons, with independent palettes. */
static void reactor_color(uint32_t palette, uint32_t fade, uint32_t fluctuation,
			  uint16_t rgb[3])
{
	rgb[0] = UINT16_MAX * palette / 4095;
	rgb[1] = UINT16_MAX;
	rgb[2] = UINT16_MAX * (4095 - palette) / 4095;
	color_to_pwm(rgb, UINT16_MAX);
	uint32_t maximum = MAX(rgb[0], MAX(rgb[1], rgb[2]));
	/* Cyan base 40%, yellow 95%; ripple adds 2..5%, rare surges 10..20%. */
	uint32_t gain = (1638 + (3890 - 1638) * palette / 4095 + fluctuation) * fade / 4095;

	for (int i = 0; i < 3; i++) {
		uint32_t normalized = maximum ? (uint32_t)rgb[i] * UINT16_MAX / maximum : 0;

		/* Brightness can exceed unity: saturation itself shifts the hue. */
		rgb[i] = MIN(normalized * gain / 4095, UINT16_MAX);
	}
}

static void bluetooth_color(int64_t now, uint32_t fluctuation)
{
	uint16_t *rgb = colors + 6;

	rgb[0] = rgb[1] = rgb[2] = 0;
	if (menu_state != HUB_LIGHT_NORMAL) {
		if (menu_state == HUB_LIGHT_MENU) {
			rgb[0] = rgb[1] = rgb[2] = UINT16_MAX;
			color_to_pwm(rgb, UINT16_MAX);
		}
	} else if (bluetooth_state == HOST_BLE_WAITING) {
		/* 100 ms at full blue PWM every three seconds; no catch-up flashes. */
		rgb[2] = (now - bluetooth_changed) % 3000 < 100 ? UINT16_MAX : 0;
	} else if (bluetooth_state == HOST_BLE_CONNECTED) {
		/* A link alone has no profile; keep blue steady until a service is used. */
		rgb[2] = 26214;
	} else if (bluetooth_state == HOST_BLE_PYBRICKS || bluetooth_state == HOST_BLE_NATIVE) {
		reactor_color(palette_blend(&bluetooth_palette, now), 4095, fluctuation, rgb);
	}
}

int hub_light_select(enum host_mode selected, enum hub_light_menu menu, int64_t now)
{
	if ((selected != HOST_PYBRICKS && selected != HOST_SHELL) ||
	    menu < HUB_LIGHT_NORMAL || menu > HUB_LIGHT_BUSY) {
		return -EINVAL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	int err = mode == OFF ? -ESHUTDOWN : !device_is_ready(leds) ? -ENODEV : 0;
	if (!err) {
		menu_state = menu;
		bluetooth_manual = false;
		bluetooth_color(now, 0);
		err = write_colors();
		if (!err) {
			center_palette = (struct palette){.from = palette_blend(&center_palette, now),
				.target = selected == HOST_SHELL ? 4095 : 0, .changed = now};
			mode = ANIMATED;
		}
	}
	k_mutex_unlock(&lock);
	return err;
}

int64_t hub_light_update(int64_t now, enum host_ble_status bluetooth)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool changed = mode != OFF && bluetooth != bluetooth_state;
	if (changed) {
		uint32_t target = bluetooth == HOST_BLE_NATIVE ? 4095 : 0;
		bool had_palette = bluetooth_state == HOST_BLE_NATIVE ||
			bluetooth_state == HOST_BLE_PYBRICKS;
		bluetooth_palette = (struct palette){
			.from = had_palette ? palette_blend(&bluetooth_palette, now) : target,
			.target = target, .changed = now};
		bluetooth_state = bluetooth;
		bluetooth_changed = now;
		bluetooth_manual = false;
	}
	bool animate_bt = !bluetooth_manual && menu_state == HUB_LIGHT_NORMAL &&
		(bluetooth_state == HOST_BLE_WAITING || bluetooth_state == HOST_BLE_PYBRICKS ||
		 bluetooth_state == HOST_BLE_NATIVE);
	bool animate = mode != OFF && (mode == ANIMATED || animate_bt);
	if (changed || (animate && now >= next_frame)) {
		if (started < 0) {
			started = now;
			next_flare = now + 1500;
			next_surge = now + 3500;
		}
		int64_t elapsed = now - started;
		uint32_t fade = ease(MIN(elapsed, 1200), 1200);
		uint32_t fluctuation = reactor_fluctuation(now);
		if (mode == ANIMATED) {
			reactor_color(palette_blend(&center_palette, now), fade, fluctuation, colors);
			memcpy(colors + 3, colors, 3 * sizeof(*colors));
		}
		if (!bluetooth_manual) {
			bluetooth_color(now, fluctuation);
		}
		int err = device_is_ready(leds) ? write_colors() : -ENODEV;

		if (err) {
			mode = MANUAL;
			bluetooth_manual = true;
			animate = false;
			LOG_ERR("Readiness light stopped: %d", err);
		}
		/* 50 frames/s: one 97-byte latch per frame, no catch-up burst after a delay. */
		next_frame = started + (elapsed / 20 + 1) * 20;
	}
	int64_t deadline = animate ? next_frame : INT64_MAX;

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
		} else {
			bluetooth_manual = true;
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
		memset(colors, 0, sizeof(colors));
		err = write_colors();
	}
	k_mutex_unlock(&lock);
	return err;
}
