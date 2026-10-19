// SPDX-License-Identifier: Apache-2.0

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include "hub_light.h"
#if DT_NODE_HAS_COMPAT(DT_NODELABEL(hub_leds), lego_hub_pwm_rgb)
#include <zephyr/drivers/pwm.h>

static const struct pwm_dt_spec rgb_channels[] = {
	PWM_DT_SPEC_GET_BY_IDX(DT_NODELABEL(hub_leds), 0),
	PWM_DT_SPEC_GET_BY_IDX(DT_NODELABEL(hub_leds), 1),
	PWM_DT_SPEC_GET_BY_IDX(DT_NODELABEL(hub_leds), 2),
};
BUILD_ASSERT(DT_PROP_LEN(DT_NODELABEL(hub_leds), pwms) == 3);

static bool lights_ready(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(rgb_channels); i++) {
		if (!pwm_is_ready_dt(&rgb_channels[i])) return false;
	}
	return true;
}

static bool light_present(uint32_t light) { return light == BP_LIGHT_CENTER; }

static int write_frame(const uint16_t frame[9])
{
	int first_error = 0;
	for (size_t i = 0; i < ARRAY_SIZE(rgb_channels); i++) {
		const struct pwm_dt_spec *channel = &rgb_channels[i];
		uint32_t pulse = ((uint64_t)channel->period * frame[i] + 32767) / UINT16_MAX;
		int err = pwm_set_pulse_dt(channel, pulse);
		/* Still attempt the remaining channels, especially when turning off. */
		if (!first_error) first_error = err;
	}
	return first_error;
}
#else
#include "led_tlc5955.h"

static const struct device *const leds = DEVICE_DT_GET(DT_NODELABEL(hub_leds));
BUILD_ASSERT(DT_NODE_CHILD_IDX(DT_NODELABEL(status_bottom)) ==
	     DT_NODE_CHILD_IDX(DT_NODELABEL(status_top)) + 1);
BUILD_ASSERT(DT_NODE_CHILD_IDX(DT_NODELABEL(bluetooth_led)) ==
	     DT_NODE_CHILD_IDX(DT_NODELABEL(status_top)) + 2);

static bool lights_ready(void) { return device_is_ready(leds); }
static bool light_present(uint32_t light) { return light < BP_LIGHT_COUNT; }

static int write_frame(const uint16_t frame[9])
{
	return tlc5955_set_colors16(leds, DT_NODE_CHILD_IDX(DT_NODELABEL(status_top)), 3, frame);
}
#endif

LOG_MODULE_REGISTER(hub_light);

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
static atomic_t bluetooth_retry_event;
static int64_t bluetooth_retry_until;
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

static struct k_spinlock application_lock;
static struct application_light {
	struct bp_light_state state;
	uint16_t pwm[3];
	uint32_t revision;
	bool releasing;
	struct bp_light_frame *frames;
	uint64_t period;
	int64_t started, deadline;
} application_lights[BP_LIGHT_COUNT];
static bool disabled;

BUILD_ASSERT(sizeof(struct bp_light_frame) * BP_LIGHT_MAX_FRAMES == 2048);
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

/* application_lock is held. All buffers belong to the service, not its caller. */
static void stop_animation(struct application_light *light)
{
	k_free(light->frames);
	light->frames = NULL;
	light->state.animating = false;
	light->state.frames = light->state.frame = 0;
}

static void request_color(struct application_light *light, const uint8_t rgb[3])
{
	memcpy(light->state.rgb, rgb, sizeof(light->state.rgb));
	for (size_t i = 0; i < 3; i++) {
		light->pwm[i] = rgb[i] * 257U;
	}
	color_to_pwm(light->pwm, UINT16_MAX);
	light->state.owned = true;
	light->state.pending = true;
	light->revision++;
}

/* At most 256 cells per light, one output latch per update, no replay burst. */
static int64_t advance_animations(int64_t now)
{
	int64_t deadline = INT64_MAX;
	for (size_t i = 0; i < BP_LIGHT_COUNT; i++) {
		struct application_light *light = &application_lights[i];
		if (!light->state.animating) {
			continue;
		}
		if (light->started < 0) {
			light->started = now;
			light->deadline = now + light->frames[0].duration_ms;
		} else if (now >= light->deadline) {
			uint64_t phase = (uint64_t)(now - light->started) % light->period;
			size_t frame = 0;
			while (phase >= light->frames[frame].duration_ms) {
				phase -= light->frames[frame++].duration_ms;
			}
			light->state.frame = frame;
			request_color(light, light->frames[frame].rgb);
			light->deadline = now + light->frames[frame].duration_ms - phase;
		}
		deadline = MIN(deadline, light->deadline);
	}
	return deadline;
}

static int write_colors(void)
{
	uint16_t frame[ARRAY_SIZE(colors)];
	uint32_t revision[BP_LIGHT_COUNT];
	bool visible[BP_LIGHT_COUNT];
	memcpy(frame, colors, sizeof(frame));
	k_spinlock_key_t key = k_spin_lock(&application_lock);
	for (size_t i = 0; i < BP_LIGHT_COUNT; i++) {
		revision[i] = application_lights[i].revision;
		visible[i] = application_lights[i].state.owned && !application_lights[i].releasing &&
			mode != OFF && menu_state == HUB_LIGHT_NORMAL;
		if (visible[i]) {
			memcpy(frame + (i == BP_LIGHT_CENTER ? 0 : 6), application_lights[i].pwm,
			       sizeof(application_lights[i].pwm));
			if (i == BP_LIGHT_CENTER) {
				memcpy(frame + 3, application_lights[i].pwm, sizeof(application_lights[i].pwm));
			}
		}
	}
	k_spin_unlock(&application_lock, key);
	int err = write_frame(frame);
	key = k_spin_lock(&application_lock);
	for (size_t i = 0; i < BP_LIGHT_COUNT; i++) {
		struct bp_light_state *state = &application_lights[i].state;
		if (revision[i] != application_lights[i].revision) {
			continue;
		}
		state->error = err;
		state->valid = !err;
		state->pending = false;
		if (err) {
			state->errors += state->errors != UINT32_MAX;
			state->animating = false;
		} else {
			state->applied_ms = k_uptime_get();
			state->visible = visible[i];
			if (application_lights[i].releasing) {
				state->owned = false;
				application_lights[i].releasing = false;
			}
		}
	}
	k_spin_unlock(&application_lock, key);
	return err;
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
	} else if (now < bluetooth_retry_until) {
		/* Full PWM makes recovery visible without changing the selected mode. */
		rgb[0] = UINT16_MAX;
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

void hub_light_ble_retry(void) { atomic_set(&bluetooth_retry_event, 1); }

int hub_light_select(enum host_mode selected, enum hub_light_menu menu, int64_t now)
{
	if ((selected != HOST_PYBRICKS && selected != HOST_SHELL) ||
	    menu < HUB_LIGHT_NORMAL || menu > HUB_LIGHT_BUSY) {
		return -EINVAL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	int err = mode == OFF ? -ESHUTDOWN : !lights_ready() ? -ENODEV : 0;
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
	k_spinlock_key_t key = k_spin_lock(&application_lock);
	int64_t application_deadline = advance_animations(now);
	bool pending = application_lights[0].state.pending || application_lights[1].state.pending;
	k_spin_unlock(&application_lock, key);
	bool changed = mode != OFF && bluetooth != bluetooth_state;
	bool retry_event = atomic_set(&bluetooth_retry_event, 0);
	if (retry_event && mode != OFF) bluetooth_retry_until = now + 100;
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
		 bluetooth_state == HOST_BLE_NATIVE || now < bluetooth_retry_until);
	bool animate = mode != OFF && (mode == ANIMATED || animate_bt);
	bool readiness_frame = changed || retry_event || (animate && now >= next_frame);
	if (readiness_frame) {
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
		/* Readiness runs at 50 Hz independently of application frame intervals. */
		next_frame = started + (elapsed / 20 + 1) * 20;
	}
	if (readiness_frame || pending) {
		int err = lights_ready() ? write_colors() : -ENODEV;

		if (err) {
			mode = MANUAL;
			bluetooth_manual = true;
			animate = false;
			application_deadline = INT64_MAX;
			LOG_ERR("Readiness light stopped: %d", err);
		}
	}
	int64_t deadline = MIN(animate ? next_frame : INT64_MAX, application_deadline);

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
	if (!light_present(center ? BP_LIGHT_CENTER : BP_LIGHT_BLUETOOTH)) return -ENODEV;
	k_mutex_lock(&lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&application_lock);
	bool owned = application_lights[center ? BP_LIGHT_CENTER : BP_LIGHT_BLUETOOTH].state.owned;
	k_spin_unlock(&application_lock, key);
	int err = mode == OFF ? -ESHUTDOWN : !lights_ready() ? -ENODEV : 0;
	if (!err && owned) {
		err = -EBUSY;
	}

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
	k_spinlock_key_t key = k_spin_lock(&application_lock);
	disabled = true;
	for (size_t i = 0; i < BP_LIGHT_COUNT; i++) {
		stop_animation(&application_lights[i]);
	}
	k_spin_unlock(&application_lock, key);
	mode = OFF;
	int err = -ENODEV;

	if (lights_ready()) {
		memset(colors, 0, sizeof(colors));
		err = write_colors();
	}
	k_mutex_unlock(&lock);
	return err;
}

#ifdef CONFIG_BRICKPHYR_LIGHT
int hub_light_write(uint32_t light, const uint8_t rgb[3])
{
	if (light >= BP_LIGHT_COUNT || !rgb) {
		return -EINVAL;
	}
	if (!light_present(light) || !lights_ready()) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&application_lock);
	struct bp_light_state *state = &application_lights[light].state;
	int err = disabled ? -ESHUTDOWN : state->pending || application_lights[light].releasing ? -EBUSY : 0;
	if (!err) {
		stop_animation(&application_lights[light]);
		request_color(&application_lights[light], rgb);
	}
	k_spin_unlock(&application_lock, key);
	return err;
}

int hub_light_animate(uint32_t light, const struct bp_light_frame *frames, size_t count)
{
	if (light >= BP_LIGHT_COUNT || !frames || !count || count > BP_LIGHT_MAX_FRAMES) {
		return -EINVAL;
	}
	uint64_t period = 0;
	for (size_t i = 0; i < count; i++) {
		if (!frames[i].duration_ms || frames[i].duration_ms > INT32_MAX) {
			return -EINVAL;
		}
		period += frames[i].duration_ms;
	}
	if (!light_present(light) || !lights_ready()) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&application_lock);
	struct application_light *value = &application_lights[light];
	int err = disabled ? -ESHUTDOWN : value->state.pending || value->releasing ? -EBUSY : 0;
	if (!err) {
		/* k_malloc never waits. Allocation, bounded copy and publication share
		 * the lock: aborting an application cannot leak an unpublished buffer.
		 * Validate user ranges/alignment before this critical section. */
		struct bp_light_frame *copy = k_malloc(count * sizeof(*frames));
		if (!copy) {
			err = -ENOMEM;
		} else {
			memcpy(copy, frames, count * sizeof(*frames));
			stop_animation(value);
			value->frames = copy;
			value->period = period;
			value->started = -1;
			value->state.frames = count;
			value->state.animating = true;
			request_color(value, copy[0].rgb);
		}
	}
	k_spin_unlock(&application_lock, key);
	return err;
}

int hub_light_get(uint32_t light, struct bp_light_state *value)
{
	if (light >= BP_LIGHT_COUNT || !value) {
		return -EINVAL;
	}
	if (!light_present(light) || !lights_ready()) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&application_lock);
	*value = application_lights[light].state;
	k_spin_unlock(&application_lock, key);
	return 0;
}

int hub_light_release(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&application_lock);
	bool owned[BP_LIGHT_COUNT];
	for (size_t i = 0; i < BP_LIGHT_COUNT; i++) {
		owned[i] = application_lights[i].state.owned;
		if (owned[i]) {
			stop_animation(&application_lights[i]);
			application_lights[i].releasing = true;
			application_lights[i].revision++;
		}
	}
	k_spin_unlock(&application_lock, key);
	if (mode != OFF) {
		int64_t now = k_uptime_get();
		if (owned[BP_LIGHT_CENTER]) {
			mode = ANIMATED;
			reactor_color(palette_blend(&center_palette, now), 4095, 0, colors);
			memcpy(colors + 3, colors, 3 * sizeof(*colors));
		}
		if (owned[BP_LIGHT_BLUETOOTH]) {
			bluetooth_manual = false;
			bluetooth_color(now, 0);
		}
	}
	int err = owned[BP_LIGHT_CENTER] || owned[BP_LIGHT_BLUETOOTH] ? write_colors() : 0;
	k_mutex_unlock(&lock);
	return err;
}
#endif
