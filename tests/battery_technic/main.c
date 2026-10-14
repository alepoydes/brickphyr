// SPDX-License-Identifier: Apache-2.0
/* Production battery acquisition with the Technic board coefficients and
 * synthetic ADC/GPIO inputs. This does not validate analogue accuracy. */
#include "hub_battery.h"

#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define BATTERY DT_NODELABEL(hub_battery)
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
static const struct gpio_dt_spec chemistry = GPIO_DT_SPEC_GET(BATTERY, chemistry_gpios);

static int set_alkaline_before_battery_init(void)
{
	int err = gpio_pin_configure_dt(&chemistry, GPIO_INPUT);
	return err ? err : gpio_emul_input_set(chemistry.port, chemistry.pin, 1);
}
SYS_INIT(set_alkaline_before_battery_init, APPLICATION, 1);

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
		zassert_true(k_uptime_get() < deadline, "No new battery pair");
	} while (value.sequence == previous);
	zassert_ok(value.error);
	return value;
}

ZTEST(battery_technic, test_alkaline_acquisition)
{
	struct bp_battery_sample value = snapshot();
	zassert_equal(value.type, BP_BATTERY_ALKALINE);
	zassert_equal(value.error, -EINPROGRESS);
	zassert_equal(value.temperature_error, -ENOTSUP);
	zassert_equal(value.temperature_failures, 0);
	zassert_equal(value.temperature_ms, 0);
	zassert_ok(adc_emul_const_raw_value_set(adc, 3, 1004));
	zassert_ok(adc_emul_const_raw_value_set(adc, 2, 3072));

	k_msleep(1000);
	value = next(0);
	zassert_equal(value.current_raw, 1004);
	zassert_equal(value.voltage_raw, 3072);
	zassert_equal(value.current_ma, 1044);
	zassert_equal(value.voltage_mv, 7996);
	zassert_false(value.low);
	zassert_false(value.critical);
	zassert_equal(value.temperature_error, -ENOTSUP);
	zassert_equal(value.temperature_failures, 0);

	zassert_ok(adc_emul_const_raw_value_set(adc, 3, 0));
	value = next(value.sequence);
	zassert_equal(value.current_ma, 20);
	zassert_equal(value.voltage_mv, 7228);
	zassert_equal(value.temperature_error, -ENOTSUP);
	zassert_equal(value.temperature_failures, 0);

	/* About 5.8 V is below Li-ion critical but above alkaline low. */
	zassert_ok(adc_emul_const_raw_value_set(adc, 2, 2464));
	int64_t deadline = k_uptime_get() + 5000;
	do {
		k_msleep(10);
		value = snapshot();
		zassert_true(k_uptime_get() < deadline, "Filter did not settle");
	} while (value.average_mv >= 5900);
	zassert_false(value.low);
	zassert_false(value.critical);

	zassert_ok(adc_emul_const_raw_value_set(adc, 2, 2000));
	deadline = k_uptime_get() + 5000;
	do {
		k_msleep(10);
		value = snapshot();
		zassert_true(k_uptime_get() < deadline, "Alkaline critical state did not settle");
	} while (!value.critical);
	zassert_true(value.low);
	zassert_equal(value.temperature_error, -ENOTSUP);
	zassert_equal(value.temperature_failures, 0);
}

ZTEST_SUITE(battery_technic, NULL, NULL, NULL, NULL, NULL);
