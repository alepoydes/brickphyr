// SPDX-License-Identifier: Apache-2.0

#include <stdlib.h>
#include <string.h>
#include <zephyr/drivers/led.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>
#include "hub.h"
#include "input_resistor_ladder.h"

LOG_MODULE_REGISTER(hub);

static const struct device *const leds = DEVICE_DT_GET(DT_NODELABEL(hub_leds));
static const uint16_t codes[] = {INPUT_KEY_ENTER, INPUT_KEY_LEFT, INPUT_KEY_RIGHT,
				 INPUT_KEY_BLUETOOTH};
static const char *const names[] = {"center", "left", "right", "bluetooth"};
static atomic_t buttons;
static atomic_t presses[ARRAY_SIZE(codes)];
static K_SEM_DEFINE(button_changed, 0, 1);

static void button_event(struct input_event *event, void *user_data)
{
	if (event->type != INPUT_EV_KEY) {
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(codes); i++) {
		if (event->code == codes[i]) {
			atomic_set_bit_to(&buttons, i, event->value != 0);
			if (event->value) {
				atomic_inc(&presses[i]);
			}
			k_sem_give(&button_changed);
			break;
		}
	}
}

INPUT_CALLBACK_DEFINE_NAMED(DEVICE_DT_GET(DT_NODELABEL(center_keys)), button_event, NULL, center);
INPUT_CALLBACK_DEFINE_NAMED(DEVICE_DT_GET(DT_NODELABEL(arrow_keys)), button_event, NULL, arrows);

static int center_color(const uint8_t color[3])
{
	int err = led_set_color(leds, DT_NODE_CHILD_IDX(DT_NODELABEL(status_top)), 3, color);

	if (err) {
		return err;
	}
	return led_set_color(leds, DT_NODE_CHILD_IDX(DT_NODELABEL(status_bottom)), 3, color);
}

static void shutdown(void)
{
	if (device_is_ready(leds)) {
		const uint8_t off[] = {0, 0, 0};

		center_color(off);
		led_set_color(leds, DT_NODE_CHILD_IDX(DT_NODELABEL(bluetooth_led)), 3, off);
	}
	/* Use the same context registry as Zephyr's USB shell, including the
	 * standard automatically initialized CDC device. No private CDC symbols.
	 */
	STRUCT_SECTION_FOREACH(usbd_context, ctx) {
		if (ctx->dev == DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0))) {
			int err = usbd_disable(ctx);

			if (err) {
				LOG_ERR("USB disconnect during shutdown failed: %d", err);
			}
		}
	}
	hub_shutdown();
}

static int cmd_shutdown(const struct shell *shell, size_t argc, char **argv)
{
	shutdown();
	return 0;
}

static int cmd_buttons(const struct shell *shell, size_t argc, char **argv)
{
	if (!device_is_ready(DEVICE_DT_GET(DT_NODELABEL(center_keys))) ||
	    !device_is_ready(DEVICE_DT_GET(DT_NODELABEL(arrow_keys)))) {
		shell_error(shell, "Button driver unavailable.");
		return -ENODEV;
	}
	const struct device *ladders[] = {DEVICE_DT_GET(DT_NODELABEL(center_keys)),
					 DEVICE_DT_GET(DT_NODELABEL(arrow_keys))};

	for (size_t i = 0; i < ARRAY_SIZE(ladders); i++) {
		struct resistor_ladder_sample sample = resistor_ladder_get_sample(ladders[i]);

		shell_print(shell, "%s: ADC %d, error %d, failures %u", ladders[i]->name,
			    sample.raw, sample.error, sample.failures);
	}
	atomic_val_t state = atomic_get(&buttons);

	for (size_t i = 0; i < ARRAY_SIZE(codes); i++) {
		shell_print(shell, "%s: %s, presses: %ld", names[i],
			    state & BIT(i) ? "down" : "up", (long)atomic_get(&presses[i]));
	}
	return 0;
}

static int cmd_led(const struct shell *shell, size_t argc, char **argv)
{
	bool center = !strcmp(argv[1], "center");
	uint8_t color[3];

	if (!center && strcmp(argv[1], "bluetooth")) {
		shell_error(shell, "Use center or bluetooth.");
		return -EINVAL;
	}
	for (int i = 0; i < 3; i++) {
		int err = 0;
		unsigned long value = shell_strtoul(argv[i + 2], 10, &err);

		if (err || value > UINT8_MAX) {
			shell_error(shell, "RGB values must be integers from 0 to 255.");
			return -EINVAL;
		}
		color[i] = value;
	}
	if (!device_is_ready(leds)) {
		shell_error(shell, "LED driver unavailable.");
		return -ENODEV;
	}
	int err = center ? center_color(color) :
		led_set_color(leds, DT_NODE_CHILD_IDX(DT_NODELABEL(bluetooth_led)), 3, color);

	if (err) {
		shell_error(shell, "LED write failed: %d", err);
	}
	return err;
}

SHELL_CMD_ARG_REGISTER(shutdown, NULL, "Switch off hub power.", cmd_shutdown, 1, 0);
SHELL_CMD_ARG_REGISTER(buttons, NULL, "Button states and press counters since boot.", cmd_buttons, 1, 0);
SHELL_CMD_ARG_REGISTER(led, NULL, "led <center|bluetooth> <red> <green> <blue> (0..255).", cmd_led, 5, 0);

void hub_run(void)
{
	if (device_is_ready(leds)) {
		const uint8_t ready[] = {0, 16, 0};
		int err = center_color(ready);

		if (err) {
			LOG_ERR("Startup light failed: %d", err);
		}
	} else {
		LOG_ERR("LED driver unavailable");
	}
	int64_t pressed_at = -1;
	atomic_val_t center_presses = 0;

	for (;;) {
		bool down = atomic_test_bit(&buttons, 0);
		atomic_val_t count = atomic_get(&presses[0]);

		/* A release/repress between wakeups must restart the hold interval. */
		if (!down) {
			pressed_at = -1;
		} else if (pressed_at < 0 || count != center_presses) {
			pressed_at = k_uptime_get();
		}
		center_presses = count;
		int64_t remaining = pressed_at < 0 ? 2000 : 2000 - (k_uptime_get() - pressed_at);

		if (pressed_at >= 0 && remaining <= 0) {
			shutdown();
		}
		k_sem_take(&button_changed, pressed_at < 0 ? K_FOREVER : K_MSEC(remaining));
	}
}
