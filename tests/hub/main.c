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

static void check_bluetooth_lighting(void)
{
	/* Manual center stays untouched while Bluetooth follows the actual link. */
	for (int64_t now = 201000; now < 203000; now += 20) {
		unsigned int before = writes;
		zassert_equal(hub_light_update(now, HOST_BLE_WAITING), now + 20);
		zassert_equal(writes, before + 1, "One shared LED frame per tick");
		zassert_equal(duty(18) != 0, now % 1000 < 100, "One 100 ms flash each second");
		zassert_equal(duty(19), 0);
		zassert_equal(duty(20), 0);
		zassert_equal(duty(5), 0);
		zassert_equal(duty(4), 32768);
		zassert_equal(duty(3), 65535);
	}
	hub_light_update(203000, HOST_BLE_CONNECTED);
	zassert_true(duty(18) > 0);
	unsigned int before = writes;
	zassert_equal(hub_light_update(203900, HOST_BLE_CONNECTED), INT64_MAX);
	zassert_equal(writes, before, "Unclassified connection must be steady blue");

	/* The same palette must produce exactly the same animation on both buttons. */
	zassert_ok(hub_light_select(HOST_PYBRICKS, HUB_LIGHT_NORMAL, 204000));
	uint16_t previous = 0;
	bool changed = false;
	for (int64_t now = 204400; now < 205400; now += 20) {
		before = writes;
		hub_light_update(now, HOST_BLE_PYBRICKS);
		zassert_equal(writes, before + 1);
		for (int i = 0; i < 3; i++) {
			zassert_equal(duty(20 - i), duty(5 - i));
		}
		zassert_equal(duty(20), 0);
		zassert_true(duty(19) > 0 && duty(18) > 0);
		changed |= previous && previous != duty(18);
		previous = duty(18);
	}
	zassert_true(changed, "Connected Bluetooth must animate");
	/* A native BLE session can coexist with the Pybricks USB selection. */
	hub_light_update(205400, HOST_BLE_NATIVE);
	hub_light_update(205800, HOST_BLE_NATIVE);
	zassert_true(duty(20) >= 62000 && duty(19) > 0);
	zassert_equal(duty(18), 0);
	zassert_equal(duty(5), 0);
	zassert_true(duty(3) > 0);
	zassert_ok(hub_light_select(HOST_SHELL, HUB_LIGHT_MENU, 206000));
	for (int64_t now = 206000; now < 206400; now += 20) {
		hub_light_update(now, HOST_BLE_NATIVE);
		zassert_equal(duty(20), 65535);
		zassert_equal(duty(19), 12079);
		zassert_equal(duty(18), 16448);
	}
	zassert_ok(hub_light_select(HOST_SHELL, HUB_LIGHT_BUSY, 206400));
	hub_light_update(206400, HOST_BLE_NATIVE);
	for (int i = 0; i < 3; i++) {
		zassert_equal(duty(20 - i), 0, "Busy menu must blank even during a BLE connection");
	}
	zassert_ok(hub_light_select(HOST_SHELL, HUB_LIGHT_NORMAL, 206400));
	hub_light_update(206420, HOST_BLE_NATIVE);
	for (int i = 0; i < 3; i++) {
		zassert_equal(duty(20 - i), duty(5 - i));
	}
	const uint16_t manual[] = {1, 2, 3};
	zassert_ok(hub_light_pwm(false, manual));
	hub_light_update(206440, HOST_BLE_NATIVE);
	for (int i = 0; i < 3; i++) {
		zassert_equal(duty(20 - i), manual[i]);
	}
	hub_light_update(206460, HOST_BLE_WAITING);
	zassert_equal(duty(18), UINT16_MAX, "Disconnect must resume automatic indication");
	hub_light_update(206480, HOST_BLE_OFF);
	for (int i = 0; i < 3; i++) {
		zassert_equal(duty(20 - i), 0, "Disabled radio must not indicate waiting");
	}
	/* A link change while the menu is open must not replace its white marker. */
	zassert_ok(hub_light_select(HOST_SHELL, HUB_LIGHT_MENU, 206500));
	hub_light_update(206500, HOST_BLE_WAITING);
	zassert_equal(duty(20), UINT16_MAX);
	zassert_equal(duty(19), 12079);
	zassert_equal(duty(18), 16448);
}

ZTEST(hub, test_readiness_light_and_manual_override)
{
	zassert_equal(hub_light_update(0, HOST_BLE_OFF), 20);
	zassert_equal(duty(5), 0);
	zassert_equal(duty(4), 0);
	zassert_equal(duty(3), 0);
	hub_light_update(600, HOST_BLE_OFF);
	zassert_equal(duty(5), 0);
	zassert_true(duty(3) > 0 && duty(3) < 25000);
	hub_light_update(1200, HOST_BLE_OFF);
	zassert_equal(duty(5), 0);
	zassert_within(duty(3), 26214, 1);
	zassert_true(duty(4) > 18000 && duty(4) < duty(3));
	unsigned int before = writes;

	zassert_equal(hub_light_update(1210, HOST_BLE_OFF), 1220);
	zassert_equal(writes, before, "Early wake must not send another frame");
	const uint16_t bt[] = {0, 0, 4112};

	zassert_ok(hub_light_pwm(false, bt));
	/* Synthetic time exercises both colors through real animation/SPI code.
	 * Check visible behavior, without depending on libc's random sequence.
	 */
	for (int palette = 0; palette < 2; palette++) {
		int dominant = palette ? 5 : 3;
		int absent = palette ? 3 : 5;
		unsigned int peaks = 0, quiet = 0, changing = 0;
		bool active = false, varied = false, saturated = false;
		uint16_t previous = 0;
		int64_t last_peak = 0, last_gap = 0;
		int64_t start = palette ? 21600 : 1220;

		if (palette) {
			zassert_ok(hub_light_select(HOST_SHELL, HUB_LIGHT_MENU, 21200));
			zassert_equal(duty(20), UINT16_MAX, "Menu white must use full corrected PWM");
			zassert_equal(duty(19), 12079);
			zassert_equal(duty(18), 16448);
			hub_light_update(21220, HOST_BLE_OFF);
			zassert_true(duty(3) > duty(5), "Mode transition must start near cyan");
		}
		for (int64_t now = start; now < start + 20000; now += 20) {
			before = writes;
			zassert_equal(hub_light_update(now, HOST_BLE_OFF), now + 20);
			zassert_equal(writes, before + 1, "Both center LEDs must latch together");
			for (int i = 0; i < 3; i++) {
				zassert_equal(duty(5 - i), duty(8 - i));
			}
			zassert_equal(duty(absent), 0, "Flares must not add an absent channel");
			zassert_true(duty(dominant) >= (palette ? 62000 : 26000),
				     "Steady light must remain visible");
			zassert_true(duty(4) > 0);
			/* Observe green for yellow because red clips near the base level. */
			uint16_t level = duty(palette ? 4 : 3);
			bool peak = level > (palette ? 13000 : 33000);

			if (peak && !active) {
				int64_t gap = now - last_peak;

				varied |= last_gap && gap != last_gap;
				last_gap = last_peak ? gap : 0;
				last_peak = now;
				peaks++;
			}
			active = peak;
			quiet += !peak;
			changing += level != previous;
			previous = level;
			zassert_true(level < (palette ? 16000 : 50000), "Surges must stay restrained");
			if (palette && level > 12500) {
				saturated = true;
				zassert_equal(duty(5), UINT16_MAX,
					      "Clip red only; green must keep accumulating");
			}
			zassert_equal(duty(14), UINT16_MAX, "Charging must remain disabled");
			if (!palette) {
				zassert_equal(duty(18), 4112, "Animation must preserve Bluetooth color");
			}
		}
		zassert_true(peaks >= 3 && peaks <= 12 && quiet > 800, "Expected rare surges");
		zassert_true(changing > 850, "Small fluctuations must continue between surges");
		zassert_true(varied, "Surges must not follow a fixed period");
		if (palette) {
			zassert_true(saturated, "Expected per-channel clipping during surges");
		}
	}
	zassert_ok(hub_light_select(HOST_SHELL, HUB_LIGHT_NORMAL, 41600));
	zassert_equal(duty(18), 0);
	zassert_equal(duty(19), 0);
	zassert_equal(duty(20), 0);
	before = writes;
	hub_light_update(100000, HOST_BLE_OFF);
	zassert_equal(writes, before + 1, "Delayed frames must not cause catch-up bursts");
	/* The same board gains must apply to both kinds of RGB lights. */
	const uint8_t manual[] = {255, 255, 255};
	const uint16_t white_pwm[] = {65535, 12079, 16448};

	zassert_ok(hub_light_manual(true, manual));
	zassert_ok(hub_light_manual(false, manual));
	for (int i = 0; i < 3; i++) {
		zassert_equal(duty(5 - i), white_pwm[i]);
		zassert_equal(duty(8 - i), white_pwm[i]);
		zassert_equal(duty(20 - i), white_pwm[i]);
	}
	const uint8_t half[] = {128, 128, 128};

	zassert_ok(hub_light_manual(true, half));
	for (int i = 0; i < 3; i++) {
		zassert_true(duty(5 - i) > white_pwm[i] * 24 / 100 &&
			     duty(5 - i) < white_pwm[i] * 26 / 100,
			     "Half RGB should give approximately quarter duty with gamma 2");
	}
	const uint16_t raw[] = {0, 32768, 65535};

	zassert_ok(hub_light_pwm(true, raw));
	zassert_ok(hub_light_pwm(false, raw));
	before = writes;
	zassert_equal(hub_light_update(200000, HOST_BLE_OFF), INT64_MAX);
	zassert_equal(writes, before);
	for (int i = 0; i < 3; i++) {
		zassert_equal(duty(5 - i), raw[i]);
		zassert_equal(duty(8 - i), raw[i]);
		zassert_equal(duty(20 - i), raw[i]);
	}
	check_bluetooth_lighting();
	zassert_ok(hub_light_off());
	before = writes;
	zassert_equal(hub_light_update(300000, HOST_BLE_WAITING), INT64_MAX);
	zassert_equal(hub_light_update(300020, HOST_BLE_NATIVE), INT64_MAX);
	zassert_equal(hub_light_manual(true, manual), -ESHUTDOWN);
	zassert_equal(hub_light_manual(false, manual), -ESHUTDOWN);
	zassert_equal(hub_light_pwm(true, raw), -ESHUTDOWN);
	zassert_equal(hub_light_pwm(false, raw), -ESHUTDOWN);
	zassert_equal(hub_light_select(HOST_PYBRICKS, HUB_LIGHT_MENU, 300000), -ESHUTDOWN);
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
