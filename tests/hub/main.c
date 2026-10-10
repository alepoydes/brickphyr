// SPDX-License-Identifier: Apache-2.0

#include <string.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/input/input.h>
#include <zephyr/ztest.h>
#include <zephyr/fff.h>
#include "input_resistor_ladder.h"

DEFINE_FFF_GLOBALS;

static uint8_t frames[3][97];
static uint8_t last_frame[97];
static unsigned int writes;
static int spi_error;
static uint32_t keys;
static bool center_down;

static int receive_frame(const struct emul *emul, const struct spi_config *cfg,
			 const struct spi_buf_set *tx, const struct spi_buf_set *rx)
{
	if (spi_error) {
		return spi_error;
	}
	zassert_equal(tx->count, 1);
	zassert_equal(tx->buffers[0].len, 97);
	memcpy(last_frame, tx->buffers[0].buf, sizeof(last_frame));
	if (writes < ARRAY_SIZE(frames)) {
		memcpy(frames[writes], last_frame, sizeof(last_frame));
	}
	writes++;
	return 0;
}

static int emul_init(const struct emul *emul, const struct device *bus)
{
	return 0;
}

static const struct spi_emul_api spi_api = {.io = receive_frame};
EMUL_DT_DEFINE(DT_NODELABEL(test_leds), emul_init, NULL, NULL, &spi_api, NULL);

static void key_event(struct input_event *event, void *data)
{
	if (event->code == INPUT_KEY_ENTER) {
		center_down = event->value != 0;
		return;
	}
	uint32_t bit = event->code == INPUT_KEY_LEFT ? BIT(0) :
		       event->code == INPUT_KEY_RIGHT ? BIT(1) : BIT(2);

	if (event->value) {
		keys |= bit;
	} else {
		keys &= ~bit;
	}
}
INPUT_CALLBACK_DEFINE(NULL, key_event, NULL);

ZTEST(hub, test_led_frames)
{
	const struct device *led = DEVICE_DT_GET(DT_NODELABEL(test_leds));
	const uint8_t rgb[] = {0x12, 0x34, 0x56};

	zassert_true(device_is_ready(led));
	zassert_equal(writes, 3);
	zassert_mem_equal(frames[0], frames[1], 97);
	/* TI control-register fields for 3.2 mA and the Pybricks display mode. */
	const uint8_t control_fields[] = {0x06, 0x7f, 0xff, 0xfe, 0x00};

	zassert_equal(frames[0][0], 1);
	zassert_equal(frames[0][1], 0x96);
	zassert_mem_equal(&frames[0][50], control_fields, sizeof(control_fields));
	for (int i = 55; i < 97; i++) {
		zassert_equal(frames[0][i], 0xff);
	}
	/* Initialization leaves only charger-disable channel 14 at full duty. */
	for (int i = 0; i < 97; i++) {
		zassert_equal(frames[2][i], i == 29 || i == 30 ? 0xff : 0);
	}
	zassert_ok(led_set_color(led, 0, 3, rgb));
	const uint8_t wire_rgb[] = {0x56, 0x56, 0x34, 0x34, 0x12, 0x12};

	zassert_mem_equal(&last_frame[7], wire_rgb, sizeof(wire_rgb));
	zassert_equal(last_frame[29], 0xff);
	zassert_equal(last_frame[30], 0xff);
	unsigned int before = writes;

	zassert_equal(led_set_color(led, 1, 3, rgb), -EINVAL);
	zassert_equal(led_set_color(led, 0, 2, rgb), -EINVAL);
	zassert_equal(writes, before);
	spi_error = -EIO;
	zassert_equal(led_set_color(led, 0, 3, rgb), -EIO);
	spi_error = 0;
	zassert_ok(led_set_brightness(led, 0, 0));
	zassert_equal(last_frame[29], 0xff);
	zassert_equal(last_frame[30], 0xff);
}

static int bad_adc(const struct device *dev, unsigned int channel, void *data, uint32_t *result)
{
	return -EIO;
}

ZTEST(hub, test_button_combinations_and_read_failure)
{
	const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
	/* Representative source-derived voltages, ordered by expected input bitmask. */
	const uint16_t samples[] = {4095, 2600, 3200, 2200, 3600, 2400, 2900, 2000};

	zassert_true(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(arrow_keys))));
	zassert_ok(adc_emul_const_raw_value_set(adc, 14, 4073));
	k_msleep(60);
	/* Measured center voltages must remain pressed across their noise range. */
	for (unsigned int value = 3141; value <= 3147; value++) {
		zassert_ok(adc_emul_const_raw_value_set(adc, 14, value));
		k_msleep(30);
		zassert_true(center_down, "Center dropped out at ADC %u", value);
	}
	zassert_ok(adc_emul_const_raw_value_set(adc, 14, 4073));
	k_msleep(30);
	zassert_false(center_down);
	for (unsigned int mask = 0; mask < ARRAY_SIZE(samples); mask++) {
		zassert_ok(adc_emul_const_raw_value_set(adc, 1, samples[mask]));
		k_msleep(30);
		zassert_equal(keys, mask, "combination %u", mask);
	}
	zassert_ok(adc_emul_raw_value_func_set(adc, 1, bad_adc, NULL));
	k_msleep(20);
	zassert_equal(keys, 0, "Unreadable ADC must release pressed keys");
	struct resistor_ladder_sample last = resistor_ladder_get_sample(
		DEVICE_DT_GET(DT_NODELABEL(arrow_keys)));

	zassert_equal(last.error, -EIO);
	zassert_true(last.failures > 0);
	zassert_ok(adc_emul_const_raw_value_set(adc, 1, 2000));
	k_msleep(30);
	zassert_equal(keys, 7);
	zassert_ok(adc_emul_const_raw_value_set(adc, 1, 1969));
	k_msleep(20);
	zassert_equal(keys, 0, "Out-of-range voltage must not become a button press");
}

ZTEST_SUITE(hub, NULL, NULL, NULL, NULL, NULL);
