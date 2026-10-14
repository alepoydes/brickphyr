// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2025 The Pybricks Authors
/* Scaling ported from lib/pbio/drv/battery/battery_adc.c at a7b1b04ae.
 * Acquisition uses Zephyr ADC; board coefficients live in devicetree.
 * See docs/hardware.md, Hub battery, for provenance and validity limits. */
#include "hub_battery.h"
#include <math.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define BATTERY DT_NODELABEL(hub_battery)
#define PERIOD_MS 10
#define SETTLE_MS 1000
#define STALE_MS (4 * PERIOD_MS)
#define HAS_TEMPERATURE DT_PROP_HAS_NAME(BATTERY, io_channels, temperature)
BUILD_ASSERT(DT_PROP(BATTERY, current_raw_offset) >= 0 && DT_PROP(BATTERY, current_raw_offset) < 4096);
static const struct adc_dt_spec current = ADC_DT_SPEC_GET_BY_NAME(BATTERY, current);
static const struct adc_dt_spec voltage = ADC_DT_SPEC_GET_BY_NAME(BATTERY, voltage);
#if HAS_TEMPERATURE
static const struct adc_dt_spec temperature = ADC_DT_SPEC_GET_BY_NAME(BATTERY, temperature);
static int temperature_init_error;
#endif
#if DT_NODE_HAS_PROP(BATTERY, chemistry_gpios)
static const struct gpio_dt_spec chemistry = GPIO_DT_SPEC_GET(BATTERY, chemistry_gpios);
BUILD_ASSERT(UTIL_CAT(BP_BATTERY_, DT_STRING_UPPER_TOKEN(BATTERY, chemistry)) == BP_BATTERY_UNKNOWN);
#endif
/* PBIO battery.c's fixed-point moving average, scaled for our 10 ms cadence
 * instead of 5 ms: 64 samples retain the nominal 640 ms time constant.
 * Thresholds/hysteresis are from lib/pbio/sys/battery.c at the same revision. */
static uint32_t average_scaled;
static bool average_initialized;
static struct k_spinlock lock;
static atomic_t stopping;
static struct bp_battery_sample sample = {
	.error = -EINPROGRESS,
	.type = UTIL_CAT(BP_BATTERY_, DT_STRING_UPPER_TOKEN(BATTERY, chemistry)),
	.temperature_error = HAS_TEMPERATURE ? -EINPROGRESS : -ENOTSUP,
};
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

#if HAS_TEMPERATURE
static int read_temperature(uint16_t *raw, int32_t *mdegc)
{
	int err = read_channel(&temperature, raw);
	if (err) {
		return err;
	}
	if (!*raw) {
		return -ERANGE;
	}
	/* Pybricks' 103AT/MP2639A conversion: 2.4 kohm series, 7.5 kohm
	 * parallel, 10 kohm at 25 C, beta 3435 K. A nonpositive conductance is
	 * an invalid circuit reading, not a temperature. */
	float conductance = (4095.0f / *raw - 1.0f) / 2400.0f - 1.0f / 7500.0f;
	if (conductance <= 0) {
		return -ERANGE;
	}
	float kelvin = 3435.0f / (logf(1.0f / conductance / 10000.0f) + 3435.0f / 298.15f);
	/* Valid 12-bit readings bound this result within int32_t; signed storage
	 * also keeps temperatures below freezing defined. */
	*mdegc = (kelvin - 273.15f) * 1000.0f;
	return 0;
}
#endif

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
		current_ma = ((uint64_t)current_raw + DT_PROP(BATTERY, current_raw_offset)) *
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
		if (!average_initialized) {
			average_scaled = voltage_mv * 64;
			average_initialized = true;
		} else {
			average_scaled = (average_scaled * 63 + voltage_mv * 64) / 64;
		}
		sample.average_mv = average_scaled / 64;
		uint32_t critical = sample.type == BP_BATTERY_LI_ION ? 6000 : 4800;
		uint32_t low = sample.type == BP_BATTERY_LI_ION ? 6800 : 5400;
		uint32_t okay = sample.type == BP_BATTERY_LI_ION ? 7200 : 6000;
		if (sample.average_mv <= critical) sample.critical = true;
		else if (sample.average_mv >= low) sample.critical = false;
		if (sample.average_mv <= low) sample.low = true;
		else if (sample.average_mv >= okay) sample.low = false;
		sample.sample_ms = sample.read_ms;
		sample.sequence++;
	}
	k_spin_unlock(&lock, key);
#if HAS_TEMPERATURE
	/* Keep thermistor errors separate from the pair used by motor control. */
	if (!temperature_init_error) {
		uint16_t raw;
		int32_t mdegc;
		started = k_uptime_get();
		err = read_temperature(&raw, &mdegc);
		completed = k_uptime_get();
		if (!err && completed - started > STALE_MS) {
			err = -ETIMEDOUT;
		}
		key = k_spin_lock(&lock);
		sample.temperature_error = err;
		if (err) {
			sample.temperature_failures += sample.temperature_failures != UINT32_MAX;
		} else {
			sample.temperature_raw = raw;
			sample.temperature_mdegc = mdegc;
			sample.temperature_ms = completed;
		}
		k_spin_unlock(&lock, key);
	}
#endif
	if (!atomic_get(&stopping)) {
		k_work_schedule(&work, K_MSEC(PERIOD_MS));
	}
}

static int initialize(void)
{
	int err = 0;
#if DT_NODE_HAS_PROP(BATTERY, chemistry_gpios)
	/* Pybricks samples the battery connector once at boot, without a pull. */
	err = !gpio_is_ready_dt(&chemistry) ? -ENODEV : gpio_pin_configure_dt(&chemistry, GPIO_INPUT);
	if (!err) {
		int value = gpio_pin_get_dt(&chemistry);
		if (value < 0) err = value;
		else sample.type = value ? BP_BATTERY_ALKALINE : BP_BATTERY_LI_ION;
	}
#endif
	if (!err) {
		err = !adc_is_ready_dt(&current) || !adc_is_ready_dt(&voltage) ? -ENODEV :
			current.resolution != 12 || voltage.resolution != 12 ? -EINVAL :
			adc_channel_setup_dt(&current);
	}
	if (!err) {
		err = adc_channel_setup_dt(&voltage);
	}
	if (err) {
		sample.error = err;
		if (HAS_TEMPERATURE) sample.temperature_error = err;
		sample.failures = 1;
		sample.read_ms = k_uptime_get();
		return err;
	}
#if HAS_TEMPERATURE
	temperature_init_error = !adc_is_ready_dt(&temperature) ? -ENODEV :
		temperature.resolution != 12 ? -EINVAL : adc_channel_setup_dt(&temperature);
	if (temperature_init_error) {
		sample.temperature_error = temperature_init_error;
		sample.temperature_failures = 1;
	}
#endif
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
	if (!value->temperature_error && k_uptime_get() - value->temperature_ms > STALE_MS) {
		value->temperature_error = -ETIMEDOUT;
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
	if (!sample.temperature_error || sample.temperature_error == -EINPROGRESS) {
		sample.temperature_error = -ESHUTDOWN;
	}
	k_spin_unlock(&lock, key);
}
