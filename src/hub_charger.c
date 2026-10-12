// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 The Pybricks Authors
/* MP2639A control/status from lib/pbio/drv/charger/charger_mp2639a.c at
 * a7b1b04ae. Zephyr PWM/ADC and the shared TLC latch replace PBIO processes.
 * USB configuration grants the standard-port preset; VBUS alone grants none. */
#include "hub_charger.h"
#include "input_resistor_ladder.h"
#include "led_tlc5955.h"
#include <string.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define CHARGER DT_NODELABEL(hub_charger)
#define PERIOD_MS 250
#define CHARGE_MS (60 * 60 * 1000)
#define PAUSE_MS (30 * 1000)
#define USB_CONFIGURED BIT(0)
#define USB_SUSPENDED BIT(1)
static const struct device *const mode = DEVICE_DT_GET(DT_PHANDLE(CHARGER, mode_controller));
static const struct device *const ladder = DEVICE_DT_GET(DT_PHANDLE(CHARGER, status_ladder));
static const struct pwm_dt_spec iset = PWM_DT_SPEC_GET(CHARGER);
static const struct adc_dt_spec current = ADC_DT_SPEC_GET(CHARGER);
static struct k_spinlock lock;
static atomic_t usb;
static struct bp_charger_state state = {.error = -EINPROGRESS};
static int initialization_error, output_error;
static uint32_t pwm_cycles;
static int64_t next_sample, enabled_since, pause_until;
static bool enabled, requested, samples[7];
static unsigned int index, count;

void hub_charger_usb(enum hub_charger_usb_event event)
{
	switch (event) {
	case HUB_CHARGER_USB_RESET: atomic_set(&usb, 0); break;
	case HUB_CHARGER_USB_CONFIGURED: atomic_set(&usb, USB_CONFIGURED); break;
	case HUB_CHARGER_USB_SUSPEND: atomic_or(&usb, USB_SUSPENDED); break;
	case HUB_CHARGER_USB_RESUME: atomic_and(&usb, ~USB_SUSPENDED); break;
	}
}

static int set_enabled(bool active)
{
	/* Set ISET before enabling MODE; disable MODE before lowering ISET.
	 * 2% is the F4 reference's empirical standard USB 100 mA preset.
	 * The nominal 500 mA preset exceeded that budget in board IB readings;
	 * leave room for hub consumption until total input current is verified.
	 * The chip owns the charging phases and its analogue protections. */
	int err = 0;
	if (active) err = pwm_set_cycles(iset.dev, iset.channel, pwm_cycles,
		DIV_ROUND_CLOSEST(pwm_cycles * 2U, 100U), iset.flags);
	if (!err) err = tlc5955_set_reserved_channel16(mode,
		DT_PROP(CHARGER, mode_channel), active ? 0 : UINT16_MAX);
	if (!active || err) {
		int stopped = pwm_set_cycles(iset.dev, iset.channel, pwm_cycles, 0, iset.flags);
		if (!err) err = stopped;
	}
	return err;
}

/* One hub-loop owner, including its USB-powered off loop. No wait for a host,
 * no allocation and no periodic SPI transfer when the selected output is stable. */
void hub_charger_update(int vbus, int64_t now)
{
	if (vbus == 0) atomic_set(&usb, 0);
	atomic_val_t host = atomic_get(&usb);
	bool allowed = vbus == 1 && host == USB_CONFIGURED;
	if (!allowed) pause_until = 0;
	bool paused = allowed && now < pause_until;
	bool active = allowed && !paused;
	if (active && enabled && now - enabled_since >= CHARGE_MS) {
		pause_until = now + PAUSE_MS;
		active = false;
		paused = true;
	}
	/* USB suspend/removal cancels charging on the next hub turn, not after
	 * waiting for the slower status acquisition deadline. */
	if (now < next_sample && active == requested) return;
	requested = active;
	struct bp_charger_state value = state;
	value.configured = !!(host & USB_CONFIGURED);
	value.suspended = !!(host & USB_SUSPENDED);
	value.paused = paused;
	int err = initialization_error;
	if (!err && (active != enabled || output_error)) {
		output_error = err = set_enabled(active);
		if (!err) {
			enabled = active;
			enabled_since = now;
			count = index = 0;
			memset(samples, 0, sizeof(samples));
		}
	}
	value.enabled = enabled;
	value.limit_ma = enabled ? 100 : 0;
	value.sample_ms = now;
	if (!err) {
		uint16_t raw;
		struct adc_sequence sequence = {.buffer = &raw, .buffer_size = sizeof(raw)};
		err = adc_sequence_init_dt(&current, &sequence);
		if (!err) err = adc_read_dt(&current, &sequence);
		if (!err && raw >= BIT(current.resolution)) err = -ERANGE;
		if (!err) {
			value.current_raw = raw;
			/* The reference's uint16 result wraps below the zero offset. */
			value.current_ma = MAX(((int32_t)raw * 35116 >> 16) - 123, 0);
		}
	}
	struct resistor_ladder_sample signal = {0};
	if (!err) {
		signal = resistor_ladder_get_sample(ladder);
		err = signal.error ? signal.error : !signal.stable ? -EAGAIN :
			now - signal.sampled_ms > 4 * DT_PROP(DT_PHANDLE(CHARGER, status_ladder), sample_period_ms) ? -ETIMEDOUT : 0;
	}
	if (!err) {
		if (now >= next_sample + PERIOD_MS) {
			count = index = 0;
			memset(samples, 0, sizeof(samples));
		}
		samples[index] = !!(signal.pressed & BIT(DT_PROP(CHARGER, status_bit)));
		if (enabled) {
			if (count < ARRAY_SIZE(samples)) count++;
			unsigned int transitions = samples[0] != samples[ARRAY_SIZE(samples) - 1];
			for (size_t i = 1; i < ARRAY_SIZE(samples); i++) transitions += samples[i] != samples[i - 1];
			value.status = transitions > 2 ? BP_CHARGER_FAULT : samples[index] ? BP_CHARGER_CHARGING :
				count > 2 ? BP_CHARGER_COMPLETE : BP_CHARGER_DISCHARGING;
		} else {
			value.status = BP_CHARGER_DISCHARGING;
			count = 0;
		}
		index = (index + 1) % ARRAY_SIZE(samples);
	} else {
		count = index = 0;
		memset(samples, 0, sizeof(samples));
	}
	value.error = err ? err : vbus < 0 ? vbus : 0;
	if (value.error) value.failures += value.failures != UINT32_MAX;
	next_sample = now + PERIOD_MS;
	k_spinlock_key_t key = k_spin_lock(&lock);
	state = value;
	k_spin_unlock(&lock, key);
}

int hub_charger_get(struct bp_charger_state *value)
{
	if (!value) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	*value = state;
	k_spin_unlock(&lock, key);
	if (!value->error && k_uptime_get() - value->sample_ms >= 2 * PERIOD_MS) value->error = -ETIMEDOUT;
	return 0;
}

static int initialize(void)
{
	initialization_error = !device_is_ready(mode) || !device_is_ready(ladder) ||
		!pwm_is_ready_dt(&iset) || !adc_is_ready_dt(&current) ? -ENODEV : adc_channel_setup_dt(&current);
	uint64_t rate;
	if (!initialization_error) initialization_error = pwm_get_cycles_per_sec(iset.dev, iset.channel, &rate);
	if (!initialization_error) {
		pwm_cycles = DIV_ROUND_CLOSEST(rate * iset.period, NSEC_PER_SEC);
		output_error = set_enabled(false);
	}
	return initialization_error ? initialization_error : output_error;
}
SYS_INIT(initialize, APPLICATION, 80);
