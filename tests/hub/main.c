// SPDX-License-Identifier: Apache-2.0

#include <stdlib.h>
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
#include "hub_light.h"
#include "hub_menu.h"
#include "led_tlc5955.h"

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

	zassert_equal(led_set_color(led, 3, 3, rgb), -EINVAL);
	zassert_equal(led_set_color(led, 0, 2, rgb), -EINVAL);
	zassert_equal(writes, before);
	const uint16_t rgb16[] = {0x1234, 0x5678, 0x9abc};
	const uint8_t wire16[] = {0x9a, 0xbc, 0x56, 0x78, 0x12, 0x34};

	zassert_ok(tlc5955_set_colors16(led, 0, 1, rgb16));
	zassert_mem_equal(&last_frame[7], wire16, sizeof(wire16));
	before = writes;
	zassert_equal(tlc5955_set_colors16(led, 2, 2, rgb16), -EINVAL);
	zassert_equal(tlc5955_set_colors16(led, UINT32_MAX, 1, rgb16), -EINVAL);
	zassert_equal(tlc5955_set_colors16(led, 0, 0, rgb16), -EINVAL);
	zassert_equal(writes, before);
	spi_error = -EIO;
	zassert_equal(led_set_color(led, 0, 3, rgb), -EIO);
	spi_error = 0;
	zassert_ok(led_set_brightness(led, 0, 0));
	zassert_equal(last_frame[29], 0xff);
	zassert_equal(last_frame[30], 0xff);
}

static uint16_t duty(unsigned int channel)
{
	return ((uint16_t)last_frame[1 + 2 * channel] << 8) | last_frame[2 + 2 * channel];
}

ZTEST(hub, test_readiness_light_and_manual_override)
{
	zassert_equal(hub_light_update(0), 20);
	zassert_equal(duty(5), 0);
	zassert_equal(duty(4), 0);
	zassert_equal(duty(3), 0);
	hub_light_update(600);
	zassert_true(duty(5) > 0 && duty(5) < 4095);
	zassert_equal(duty(5), duty(4));
	hub_light_update(1200);
	zassert_equal(duty(5), 4095);
	zassert_equal(duty(4), 4095);
	zassert_equal(duty(3), 4095);
	unsigned int before = writes;

	zassert_equal(hub_light_update(1210), 1220);
	zassert_equal(writes, before, "Early wake must not send another frame");
	uint16_t previous[] = {duty(5), duty(4), duty(3)};
	const uint8_t bt[] = {0, 0, 16};

	zassert_ok(hub_light_manual(false, bt));
	/* Synthetic time runs a whole breath through the production SPI path. */
	for (int64_t now = 1220; now <= 4200; now += 20) {
		before = writes;
		zassert_equal(hub_light_update(now), now + 20);
		zassert_equal(writes, before + 1, "Both center LEDs must latch together");
		for (int i = 0; i < 3; i++) {
			uint16_t value = duty(5 - i);

			zassert_equal(value, duty(8 - i));
			zassert_true(value > 0 && value <= 4095);
			zassert_true(abs((int)value - previous[i]) <= 128, "Visible brightness jump");
			if (i < 2) {
				zassert_true(now <= 2700 ? value <= previous[i] : value >= previous[i]);
			}
			previous[i] = value;
		}
		if (now == 2700) {
			zassert_true(duty(3) > 4 * duty(4) && duty(4) > 4 * duty(5));
		}
		zassert_equal(duty(14), UINT16_MAX, "Charging must remain disabled");
		zassert_equal(duty(18), 16 * 257, "Breathing must preserve Bluetooth color");
	}
	zassert_equal(duty(5), 4095, "No jump at the cycle boundary");
	zassert_ok(hub_light_select(HOST_SHELL, true, 4200));
	for (int channel = 18; channel <= 20; channel++) {
		zassert_equal(duty(channel), 4095, "Menu must show white BT lighting");
	}
	/* The transition starts from the previous palette without resetting phase. */
	hub_light_update(4220);
	zassert_true(duty(4) > 4000 && duty(3) > 3900);
	hub_light_update(7200);
	zassert_true(duty(5) > duty(4) && duty(4) > 10 * duty(3), "Expected yellow");
	hub_light_update(8700);
	zassert_true(duty(5) > duty(3) && duty(3) > 4 * duty(4), "Expected pink");
	zassert_ok(hub_light_select(HOST_SHELL, false, 8700));
	zassert_equal(duty(18), 0);
	zassert_equal(duty(19), 0);
	zassert_equal(duty(20), 0);
	before = writes;
	hub_light_update(100000);
	zassert_equal(writes, before + 1, "Delayed frames must not cause catch-up bursts");
	const uint8_t manual[] = {7, 19, 31};

	zassert_ok(hub_light_manual(true, manual));
	before = writes;
	zassert_equal(hub_light_update(200000), INT64_MAX);
	zassert_equal(writes, before);
	zassert_equal(duty(5), 7 * 257);
	zassert_equal(duty(4), 19 * 257);
	zassert_equal(duty(3), 31 * 257);
	zassert_ok(hub_light_off());
	before = writes;
	zassert_equal(hub_light_update(300000), INT64_MAX);
	zassert_equal(hub_light_manual(true, manual), -ESHUTDOWN);
	zassert_equal(hub_light_manual(false, bt), -ESHUTDOWN);
	zassert_equal(hub_light_select(HOST_PYBRICKS, true, 300000), -ESHUTDOWN);
	zassert_equal(writes, before, "No relighting after shutdown");
	for (int channel = 3; channel <= 8; channel++) {
		zassert_equal(duty(channel), 0);
	}
	zassert_equal(duty(18), 0);
	zassert_equal(duty(14), UINT16_MAX);
}

ZTEST(hub, test_menu_selection_and_cancel)
{
	struct hub_menu menu = {0};
	zassert_false(hub_menu_input(&menu, INPUT_KEY_LEFT, true, HOST_SHELL));
	zassert_false(menu.open);
	zassert_false(hub_menu_input(&menu, INPUT_KEY_BLUETOOTH, true, HOST_SHELL));
	zassert_true(menu.open);
	zassert_equal(menu.selected, HOST_SHELL);
	/* A center key held before menu entry must not confirm on release. */
	zassert_false(hub_menu_input(&menu, INPUT_KEY_ENTER, false, HOST_SHELL));
	hub_menu_input(&menu, INPUT_KEY_LEFT, true, HOST_SHELL);
	zassert_equal(menu.selected, HOST_PYBRICKS);
	zassert_false(hub_menu_input(&menu, INPUT_KEY_ENTER, true, HOST_SHELL));
	zassert_true(hub_menu_input(&menu, INPUT_KEY_ENTER, false, HOST_SHELL));
	/* A busy transport leaves the preview open; release cannot retry by itself. */
	zassert_true(menu.open);
	zassert_false(hub_menu_input(&menu, INPUT_KEY_ENTER, false, HOST_SHELL));
	hub_menu_input(&menu, INPUT_KEY_BLUETOOTH, true, HOST_SHELL);
	zassert_false(menu.open);
	zassert_equal(menu.selected, HOST_SHELL);
	hub_menu_input(&menu, INPUT_KEY_BLUETOOTH, true, HOST_PYBRICKS);
	zassert_equal(menu.selected, HOST_PYBRICKS);
	hub_menu_input(&menu, INPUT_KEY_RIGHT, true, HOST_PYBRICKS);
	zassert_equal(menu.selected, HOST_SHELL);
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
