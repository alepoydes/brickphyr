// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2020 The Pybricks Authors
/* Scaling ported from lib/pbio/drv/battery/battery_adc.c at a7b1b04ae.
 * Acquisition uses Zephyr ADC; board coefficients live in devicetree.
 * See docs/hardware.md, Hub battery, for provenance and validity limits. */
#include "hub_battery.h"
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>

#define BATTERY DT_NODELABEL(hub_battery)
#define PERIOD_MS 10
#define SETTLE_MS 1000
#define STALE_MS (4 * PERIOD_MS)
static const struct adc_dt_spec current = ADC_DT_SPEC_GET_BY_NAME(BATTERY, current);
static const struct adc_dt_spec voltage = ADC_DT_SPEC_GET_BY_NAME(BATTERY, voltage);
static struct k_spinlock lock;
static atomic_t stopping;
static struct bp_battery_sample sample = {.error = -EINPROGRESS};
static void acquire(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(work, acquire);

static int read_channel(const struct adc_dt_spec *channel, uint16_t *raw)
{
	struct adc_sequence sequence = {.buffer = raw, .buffer_size = sizeof(*raw)};
	int err = adc_sequence_init_dt(channel, &sequence);
	if (!err) {
		err = adc_read_dt(channel, &sequence);
	}
	return !err && *raw >= BIT(channel->resolution) ? -ERANGE : err;
}

static void acquire(struct k_work *item)
{
	uint16_t current_raw, voltage_raw;
	uint32_t current_ma = 0, voltage_mv = 0;
	int64_t started = k_uptime_get();
	int err = read_channel(&current, &current_raw);
	if (!err) {
		err = read_channel(&voltage, &voltage_raw);
	}
	int64_t completed = k_uptime_get();
	if (!err && completed - started > STALE_MS) {
		err = -ETIMEDOUT;
	}
	if (!err) {
		/* Preserve PBIO's integer rounding: scale current before correcting
		 * the voltage drop in the hub's measurement path. */
		current_ma = (uint64_t)current_raw *
			DT_PROP(BATTERY, current_full_scale_ma) / BIT(current.resolution);
		voltage_mv = (uint64_t)voltage_raw *
			DT_PROP(BATTERY, voltage_full_scale_mv) / BIT(voltage.resolution) +
			(uint64_t)current_ma * DT_PROP(BATTERY, voltage_correction_microohms) / 1000000;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	sample.read_ms = completed;
	sample.error = err;
	if (err) {
		sample.failures += sample.failures != UINT32_MAX;
	} else {
		sample.current_ma = current_ma;
		sample.voltage_mv = voltage_mv;
		sample.current_raw = current_raw;
		sample.voltage_raw = voltage_raw;
		sample.sample_ms = sample.read_ms;
		sample.sequence++;
	}
	k_spin_unlock(&lock, key);
	if (!atomic_get(&stopping)) {
		k_work_schedule(&work, K_MSEC(PERIOD_MS));
	}
}

static int initialize(void)
{
	int err = !adc_is_ready_dt(&current) || !adc_is_ready_dt(&voltage) ? -ENODEV :
		current.resolution != 12 || voltage.resolution != 12 ? -EINVAL :
		adc_channel_setup_dt(&current);
	if (!err) {
		err = adc_channel_setup_dt(&voltage);
	}
	if (err) {
		sample.error = err;
		sample.failures = 1;
		sample.read_ms = k_uptime_get();
		return err;
	}
	/* Pybricks suppresses low readings for one second after power-on.
	 * Expose settling instead of fabricating a minimum battery voltage. */
	k_work_schedule(&work, K_MSEC(SETTLE_MS));
	return 0;
}
SYS_INIT(initialize, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

int hub_battery_get(struct bp_battery_sample *value)
{
	if (!value) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	*value = sample;
	k_spin_unlock(&lock, key);
	if (!value->error && k_uptime_get() - value->sample_ms > STALE_MS) {
		value->error = -ETIMEDOUT;
	}
	return 0;
}

void hub_battery_shutdown(void)
{
	atomic_set(&stopping, 1);
	struct k_work_sync sync;
	k_work_cancel_delayable_sync(&work, &sync);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (!sample.error || sample.error == -EINPROGRESS) {
		sample.error = -ESHUTDOWN;
	}
	k_spin_unlock(&lock, key);
}
