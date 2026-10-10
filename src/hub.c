// SPDX-License-Identifier: Apache-2.0

#include <stdlib.h>
#include <string.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>
#include "hub.h"
#include "hub_light.h"
#include "input_resistor_ladder.h"

LOG_MODULE_REGISTER(hub);

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

static void shutdown(void)
{
	int err = hub_light_off();

	if (err) {
		LOG_ERR("Lights off failed: %d", err);
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
	int err = hub_light_manual(center, color);

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
	int64_t pressed_at = -1;
	atomic_val_t center_presses = 0;

	for (;;) {
		int64_t now = k_uptime_get();
		bool down = atomic_test_bit(&buttons, 0);
		atomic_val_t count = atomic_get(&presses[0]);

		/* A release/repress between wakeups must restart the hold interval. */
		if (!down) {
			pressed_at = -1;
		} else if (pressed_at < 0 || count != center_presses) {
			pressed_at = now;
		}
		center_presses = count;
		if (pressed_at >= 0 && now - pressed_at >= 2000) {
			shutdown();
		}
		int64_t deadline = hub_light_update(now);

		if (pressed_at >= 0) {
			deadline = MIN(deadline, pressed_at + 2000);
		}
		k_sem_take(&button_changed, deadline == INT64_MAX ? K_FOREVER :
			   K_TIMEOUT_ABS_MS(deadline));
	}
}
