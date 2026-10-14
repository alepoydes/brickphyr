// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 The Pybricks Authors
/* MP2639A control/status from lib/pbio/drv/charger/charger_mp2639a.c at
 * a7b1b04ae. Zephyr PWM/ADC and the shared TLC latch replace PBIO processes.
 * A configured host grants the standard preset; CDP/DCP grants the charging
 * preset. VBUS alone grants none. */
#define __ZEPHYR_SUPERVISOR__
#include "hub_charger.h"
#include "host_usb_power.h"
#include "input_resistor_ladder.h"
#include "led_tlc5955.h"
#include <string.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/shell/shell.h>

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
static struct bp_charger_state state = {.error = -EINPROGRESS, .source = -EINPROGRESS};
static int initialization_error, output_error;
static uint32_t pwm_cycles;
static int64_t next_sample, enabled_since, pause_until;
static bool enabled, restart_inhibited, restart_pending, samples[7];
static unsigned int applied, requested;
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

static int set_output(unsigned int percent)
{
	/* Set ISET before enabling MODE; disable MODE before lowering ISET.
	 * On a failed change, quiesce both outputs: in particular a failed budget
	 * downgrade must not leave the old higher setting intentionally active.
	 * Keep the first error even if quiescing also fails. */
	int err = 0;
	if (percent) {
		err = pwm_set_cycles(iset.dev, iset.channel, pwm_cycles,
			DIV_ROUND_CLOSEST(pwm_cycles * percent, 100U), iset.flags);
		if (!err) applied = percent;
		if (!err && (!enabled || output_error)) {
			err = tlc5955_set_reserved_channel16(mode, DT_PROP(CHARGER, mode_channel), 0);
			if (!err) enabled = true;
		}
	}
	if (!percent || err) {
		int stopped = tlc5955_set_reserved_channel16(mode, DT_PROP(CHARGER, mode_channel), UINT16_MAX);
		if (!stopped) enabled = false;
		if (!err) err = stopped;
		stopped = pwm_set_cycles(iset.dev, iset.channel, pwm_cycles, 0, iset.flags);
		if (!stopped) applied = 0;
		if (!err) err = stopped;
	}
	return err;
}

/* One hub-loop owner, including its USB-powered off loop. No wait for a host,
 * no allocation and no periodic SPI transfer when the selected output is stable. */
void hub_charger_update(int vbus, int64_t now)
{
	int source = host_usb_source_update(vbus);
	if (vbus == 0) {
		atomic_set(&usb, 0);
		restart_inhibited = false;
	}
	atomic_val_t host = atomic_get(&usb);
	bool allowed = vbus == 1 && (host == USB_CONFIGURED ||
		source == BP_CHARGER_SOURCE_CDP || source == BP_CHARGER_SOURCE_DCP);
	bool charging_port = source == BP_CHARGER_SOURCE_CDP || source == BP_CHARGER_SOURCE_DCP;
	/* Reference Prime F4 presets, not a linear conversion to USB milliamps.
	 * ISET programs charge current; the chip's ILIM/VLIM protections are separate. */
	unsigned int percent = allowed ? charging_port ? 100 : 15 : 0;
	if (!allowed) {
		restart_pending = false;
		pause_until = 0;
	}
	/* Preserve the upstream long-charge workaround, but never restart over a
	 * known CHG fault. MODE stays on so the chip's fault remains observable. */
	if (allowed && !restart_pending && enabled && !restart_inhibited &&
	    now - enabled_since >= CHARGE_MS) {
		restart_pending = true;
	}
	bool paused = restart_pending && (!pause_until || now < pause_until);
	if (paused) percent = 0;
	/* Budget loss cancels charging on the next hub turn, not after waiting
	 * for the slower status acquisition deadline. */
	if (now < next_sample && percent == requested && source == state.source) return;
	requested = percent;
	struct bp_charger_state value = state;
	value.source = source;
	value.configured = !!(host & USB_CONFIGURED);
	value.suspended = !!(host & USB_SUSPENDED);
	value.paused = paused;
	value.input_budget_ma = allowed ? charging_port ? 1500 : 500 : 0;
	int err = initialization_error;
	if (!err && (percent != applied || !!percent != enabled || output_error)) {
		bool was_enabled = enabled;
		output_error = err = set_output(percent);
		if (!err && enabled != was_enabled) {
			enabled_since = now;
			count = index = 0;
			memset(samples, 0, sizeof(samples));
		}
		if (!err && restart_pending) {
			if (!enabled && !pause_until) pause_until = now + PAUSE_MS;
			else if (enabled) {
				restart_pending = false;
				pause_until = 0;
				value.restarts += value.restarts != UINT32_MAX;
			}
		}
	}
	value.enabled = enabled;
	value.iset_percent = applied;
	/* Policy events apply outputs immediately but must not accelerate the CHG
	 * sampling clock or turn duplicate observations into a hardware fault. */
	bool due = now >= next_sample;
	if (due) value.sample_ms = now;
	if (due && !err) {
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
	if (due && !err) {
		signal = resistor_ladder_get_sample(ladder);
		err = signal.error ? signal.error : !signal.stable ? -EAGAIN :
			now - signal.sampled_ms > 4 * DT_PROP(DT_PHANDLE(CHARGER, status_ladder), sample_period_ms) ? -ETIMEDOUT : 0;
	}
	if (due && !err) {
		if (now >= next_sample + PERIOD_MS) {
			count = index = 0;
			memset(samples, 0, sizeof(samples));
		}
		samples[index] = !!(signal.pressed & BIT(DT_PROP(CHARGER, status_bit)));
		if (enabled) {
			if (count < ARRAY_SIZE(samples)) count++;
			unsigned int transitions = samples[0] != samples[ARRAY_SIZE(samples) - 1];
			for (size_t i = 1; i < ARRAY_SIZE(samples); i++) transitions += samples[i] != samples[i - 1];
			/* Unfilled zero entries are not observed CHG transitions. */
			value.status = count == ARRAY_SIZE(samples) && transitions > 2 ? BP_CHARGER_FAULT : samples[index] ? BP_CHARGER_CHARGING :
				count > 2 ? BP_CHARGER_COMPLETE : BP_CHARGER_DISCHARGING;
		} else {
			value.status = BP_CHARGER_DISCHARGING;
			count = 0;
		}
		index = (index + 1) % ARRAY_SIZE(samples);
	} else if (due) {
		count = index = 0;
		memset(samples, 0, sizeof(samples));
	}
	if (!enabled) value.status = BP_CHARGER_DISCHARGING;
	if (!due && !err) err = state.error;
	value.error = err ? err : vbus < 0 ? vbus : 0;
	if (value.error) value.failures += value.failures != UINT32_MAX;
	if (!value.error && value.status == BP_CHARGER_FAULT) {
		restart_inhibited = true;
		if (state.status != BP_CHARGER_FAULT) value.faults += value.faults != UINT32_MAX;
	}
	value.reason = value.error ? BP_CHARGER_IO_ERROR : vbus == 0 ? BP_CHARGER_NO_USB :
		!allowed ? host & USB_SUSPENDED ? BP_CHARGER_USB_SUSPENDED : BP_CHARGER_WAIT_USB :
		paused ? BP_CHARGER_RESTART_PAUSE : value.status == BP_CHARGER_FAULT ?
		BP_CHARGER_HARDWARE_FAULT : BP_CHARGER_ACTIVE;
	if (due) next_sample = now + PERIOD_MS;
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
	if (k_uptime_get() - value->sample_ms >= 2 * PERIOD_MS) {
		if (!value->error) value->error = -ETIMEDOUT;
		if (value->source >= 0) value->source = -ETIMEDOUT;
		value->reason = BP_CHARGER_IO_ERROR;
	}
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
		output_error = set_output(0);
	}
	return initialization_error ? initialization_error : output_error;
}
SYS_INIT(initialize, APPLICATION, 80);

static int cmd_charger(const struct shell *shell, size_t argc, char **argv)
{
	struct bp_charger_state value;
	hub_charger_get(&value);
	static const char *const sources[] = {"none", "nonstandard", "SDP", "CDP", "DCP"};
	static const char *const reasons[] = {"no USB", "waiting for USB configuration", "USB suspended",
		"enabled", "periodic restart pause", "hardware fault", "I/O error"};
	shell_print(shell, "Source: %s (%d); USB allowance: %u mA",
		value.source < 0 ? "unavailable" : sources[value.source], value.source, value.input_budget_ma);
	shell_print(shell, "Charger: %s; ISET: %u%%; chip status: %u; error: %d",
		reasons[value.reason], value.iset_percent, value.status, value.error);
	shell_print(shell, "IB estimate: %d mA (raw %u); faults: %u; periodic restarts: %u",
		value.current_ma, value.current_raw, value.faults, value.restarts);
	return value.error;
}
SHELL_CMD_ARG_REGISTER(charger, NULL, "Charger source, allowance, applied ISET and status (read-only).", cmd_charger, 1, 0);
