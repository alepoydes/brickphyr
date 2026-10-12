// SPDX-License-Identifier: Apache-2.0
/* Production acquisition/observations with synthetic ADC voltages and errors.
 * This does not emulate the STM32 conversion watchdog or analogue accuracy. */
#include "hub_battery.h"
#include "input_resistor_ladder.h"
#include <string.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
static const struct device *const center = DEVICE_DT_GET(DT_NODELABEL(center_keys));
static uint32_t raw[2] = {1024, 3072};
static int error[2];
static unsigned int reads[2];
static bool hold;
static K_SEM_DEFINE(entered, 0, 1);
static K_SEM_DEFINE(resume, 0, 1);
static K_SEM_DEFINE(stop_requested, 0, 1);
static K_SEM_DEFINE(stop_completed, 0, 1);

static int convert(const struct device *dev, unsigned int channel, void *context, uint32_t *out)
{
	size_t index = channel - 10;
	reads[index]++;
	if (hold) {
		hold = false;
		k_sem_give(&entered);
		zassert_ok(k_sem_take(&resume, K_MSEC(200)));
	}
	*out = raw[index];
	return error[index];
}

static void stop(void *a, void *b, void *c)
{
	k_sem_take(&stop_requested, K_FOREVER);
	hub_battery_shutdown();
	k_sem_give(&stop_completed);
}
K_THREAD_DEFINE(stopper, 1024, stop, NULL, NULL, NULL, 6, 0, 0);

static struct bp_battery_sample snapshot(void)
{
	struct bp_battery_sample value;
	zassert_ok(hub_battery_get(&value));
	return value;
}

static struct bp_battery_sample next(uint32_t previous)
{
	int64_t deadline = k_uptime_get() + 100;
	struct bp_battery_sample value;
	do {
		k_msleep(1);
		value = snapshot();
		zassert_true(k_uptime_get() < deadline, "No new pair, error %d", value.error);
	} while (value.sequence == previous || value.temperature_error == -EINPROGRESS);
	zassert_ok(value.error);
	return value;
}

static void wait_center(int expected)
{
	/* Two debounced samples, each delayed by its period plus up to one
	 * scheduler tick. This fixture's tick is coarser than the F4's. */
	int64_t deadline = k_uptime_get() + 2 * DT_PROP(DT_NODELABEL(center_keys), sample_period_ms) +
		3 * k_ticks_to_ms_ceil64(1);
	while (resistor_ladder_get_key(center, INPUT_KEY_ENTER) != expected) {
		zassert_true(k_uptime_get() < deadline, "Button did not reach %d", expected);
		k_msleep(1);
	}
}

ZTEST(battery, test_acquisition)
{
	struct bp_battery_sample value = snapshot();
	zassert_equal(value.error, -EINPROGRESS);
	zassert_equal(value.temperature_error, -EINPROGRESS);
	zassert_equal(value.type, BP_BATTERY_LI_ION);
	zassert_equal(value.sequence, 0);
	zassert_equal(value.voltage_mv, 0, "Startup must not fabricate 7 V");
	zassert_equal(hub_battery_get(NULL), -EINVAL);
	zassert_ok(adc_emul_raw_value_func_set(adc, 10, convert, NULL));
	zassert_ok(adc_emul_raw_value_func_set(adc, 11, convert, NULL));
	/* Reference divider with a 10 kohm thermistor at 25 C. */
	zassert_ok(adc_emul_const_raw_value_set(adc, 8, 2625));
	zassert_ok(adc_emul_const_raw_value_set(adc, 1, 4095));
	zassert_ok(adc_emul_const_raw_value_set(adc, 14, 4073));
	k_msleep(900);
	zassert_equal(snapshot().error, -EINPROGRESS);
	zassert_equal(reads[0] + reads[1], 0);
	k_msleep(110);
	value = next(0);
	zassert_equal(value.current_raw, 1024);
	zassert_equal(value.voltage_raw, 3072);
	zassert_equal(value.current_ma, 1825);
	zassert_equal(value.voltage_mv, 7767);
	zassert_ok(value.temperature_error);
	zassert_within(value.temperature_mdegc, 25000, 1);
	zassert_true(value.sample_ms >= 1000);
	zassert_equal(value.sample_ms, value.read_ms);
	zassert_equal(resistor_ladder_get_key(center, INPUT_KEY_ENTER), 0);
	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(shell_execute_cmd(shell, "observe battery"));
	size_t length;
	const char *output = shell_backend_dummy_get_output(shell, &length);
	zassert_not_null(strstr(output, "voltage_mv=7767"));
	zassert_not_null(strstr(output, "current_ma=1825"));
	zassert_not_null(strstr(output, "temperature_raw=2625"));

	/* A failed thermistor does not disable voltage/current observations. */
	zassert_ok(adc_emul_const_raw_value_set(adc, 8, 0));
	k_msleep(25);
	struct bp_battery_sample thermal = snapshot();
	zassert_ok(thermal.error);
	zassert_true(thermal.sequence > value.sequence);
	zassert_equal(thermal.temperature_error, -ERANGE);
	zassert_equal(thermal.temperature_mdegc, value.temperature_mdegc);
	zassert_true(thermal.temperature_failures > 0);
	k_msleep(15);
	zassert_equal(snapshot().temperature_ms, thermal.temperature_ms);
	/* A high thermistor resistance is below freezing, not unsigned wrap. */
	zassert_ok(adc_emul_const_raw_value_set(adc, 8, 3000));
	k_msleep(25);
	thermal = snapshot();
	zassert_ok(thermal.temperature_error);
	zassert_true(thermal.temperature_mdegc < 0);
	zassert_ok(adc_emul_const_raw_value_set(adc, 8, 4095));
	k_msleep(25);
	zassert_equal(snapshot().temperature_error, -ERANGE);
	zassert_ok(adc_emul_const_raw_value_set(adc, 8, 2625));
	value = next(snapshot().sequence);

	/* A failed second conversion must not publish a mixed pair. */
	raw[0] = 0;
	error[1] = -EIO;
	k_msleep(25);
	struct bp_battery_sample failed = snapshot();
	zassert_equal(failed.error, -EIO);
	zassert_equal(failed.sequence, value.sequence);
	zassert_equal(failed.current_ma, value.current_ma);
	zassert_equal(failed.voltage_mv, value.voltage_mv);
	zassert_equal(failed.sample_ms, value.sample_ms);
	zassert_true(failed.read_ms > failed.sample_ms && failed.failures >= 1);
	zassert_ok(adc_emul_const_raw_value_set(adc, 14, 3145));
	k_msleep(25);
	zassert_equal(resistor_ladder_get_key(center, INPUT_KEY_ENTER), 1);
	error[0] = -ETIMEDOUT;
	k_msleep(15);
	zassert_equal(snapshot().error, -ETIMEDOUT);
	unsigned int voltage_reads = reads[1];
	k_msleep(15);
	zassert_equal(reads[1], voltage_reads, "Ignore later reads after the first failure");
	error[0] = error[1] = 0;
	raw[1] = 0;
	value = next(value.sequence);
	zassert_equal(value.voltage_mv, 0);
	zassert_equal(value.current_ma, 0);
	raw[0] = raw[1] = 4095;
	value = next(value.sequence);
	zassert_equal(value.current_ma, 7298);
	zassert_equal(value.voltage_mv, 11265);
	uint32_t failures = value.failures;

	/* Block the synthetic converter, not the reader: cache ages, and no
	 * observation starts extra ADC transactions behind the blocked one. */
	hold = true;
	zassert_ok(k_sem_take(&entered, K_MSEC(30)));
	k_msleep(45);
	value = snapshot();
	zassert_equal(value.error, -ETIMEDOUT);
	zassert_equal(value.temperature_error, -ETIMEDOUT);
	zassert_equal(value.failures, failures, "Reading stale cache must not count another I/O failure");
	unsigned int current_reads = reads[0];
	voltage_reads = reads[1];
	zassert_equal(shell_execute_cmd(shell, "observe battery"), -ETIMEDOUT);
	zassert_equal(reads[0], current_reads);
	zassert_equal(reads[1], voltage_reads);
	zassert_equal(snapshot().sample_ms, value.sample_ms);
	k_sem_give(&resume);
	value = next(value.sequence);
	zassert_equal(value.failures, failures + 1, "Reject a pair spanning an excessive delay");

	hold = true;
	zassert_ok(k_sem_take(&entered, K_MSEC(30)));
	k_sem_give(&stop_requested);
	zassert_equal(k_sem_take(&stop_completed, K_MSEC(5)), -EAGAIN);
	k_sem_give(&resume);
	zassert_ok(k_sem_take(&stop_completed, K_MSEC(100)));
	zassert_equal(snapshot().error, -ESHUTDOWN);
	zassert_equal(snapshot().temperature_error, -ESHUTDOWN);
	current_reads = reads[0];
	voltage_reads = reads[1];
	zassert_ok(adc_emul_const_raw_value_set(adc, 14, 4073));
	wait_center(0);
	zassert_equal(reads[0], current_reads);
	zassert_equal(reads[1], voltage_reads);
	zassert_equal(resistor_ladder_get_key(center, INPUT_KEY_ENTER), 0,
		"Battery shutdown must leave button ADC available for USB restart");
}
ZTEST_SUITE(battery, NULL, NULL, NULL, NULL, NULL);
