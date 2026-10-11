// SPDX-License-Identifier: Apache-2.0

#include <stdlib.h>
#include <string.h>
#include <zephyr/input/input.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include "native.h"
#include <zephyr/usb/usbd.h>
#include "hub.h"
#include "hub_light.h"
#include "hub_matrix.h"
#include "hub_menu.h"
#include "hub_power.h"
#include "input_resistor_ladder.h"

LOG_MODULE_REGISTER(hub);

static const uint16_t codes[] = {INPUT_KEY_ENTER, INPUT_KEY_LEFT, INPUT_KEY_RIGHT,
				 INPUT_KEY_BLUETOOTH};
static const char *const names[] = {"center", "left", "right", "bluetooth"};
static atomic_t buttons;

uint32_t hub_buttons(void)
{
	return atomic_get(&buttons);
}
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
	int stopped = native_shutdown();
	if (stopped) {
		LOG_ERR("Application shutdown failed: %d", stopped);
		return;
	}
	int err = hub_matrix_shutdown();
	if (err) {
		LOG_ERR("Matrix off failed: %d", err);
	}
	err = hub_light_off();

	if (err) {
		LOG_ERR("Lights off failed: %d", err);
	}
	/* Use the same context registry as Zephyr's USB shell, including the
	 * standard automatically initialized CDC device. No private CDC symbols.
	 */
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
	STRUCT_SECTION_FOREACH(usbd_context, ctx) {
		if (ctx->dev == DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0))) {
			int err = usbd_disable(ctx);

			if (err) {
				LOG_ERR("USB disconnect during shutdown failed: %d", err);
			}
		}
	}
#endif
#if defined(CONFIG_BT)
	host_ble_shutdown();
#endif
	hub_power_release();
	/* USB may keep the MCU powered. Keep the existing ADC/input driver alive,
	 * require a valid release, then reboot on a fresh debounced press. */
	bool released = false;
	const struct device *center = DEVICE_DT_GET(DT_NODELABEL(center_keys));

	for (;;) {
		int pressed = device_is_ready(center) ?
			resistor_ladder_get_key(center, INPUT_KEY_ENTER) : -ENODEV;
		if (pressed == 0) {
			released = true;
		} else if (pressed == 1 && released) {
			sys_reboot(SYS_REBOOT_COLD);
		}
		k_sem_take(&button_changed,
			K_MSEC(DT_PROP(DT_NODELABEL(center_keys), sample_period_ms)));
	}
}

static int cmd_shutdown(const struct shell *shell, size_t argc, char **argv)
{
	hub_power_request_shutdown();
	k_sem_give(&button_changed);
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
	bool raw = !strcmp(argv[1], "pwm");
	size_t target = raw ? 2 : 1;
	unsigned int maximum = raw ? UINT16_MAX : UINT8_MAX;
	uint16_t values[3];

	if (argc != target + 4) {
		shell_error(shell, "Usage: led [pwm] <center|bluetooth> <red> <green> <blue>");
		return -EINVAL;
	}
	bool center = !strcmp(argv[target], "center");

	if (!center && strcmp(argv[target], "bluetooth")) {
		shell_error(shell, "Use center or bluetooth.");
		return -EINVAL;
	}
	for (int i = 0; i < 3; i++) {
		int err = 0;
		unsigned long value = shell_strtoul(argv[target + 1 + i], 10, &err);

		if (err || value > maximum) {
			shell_error(shell, "%s values must be integers from 0 to %u.",
				    raw ? "PWM" : "RGB", maximum);
			return -EINVAL;
		}
		values[i] = value;
	}
	const uint8_t color[] = {values[0], values[1], values[2]};
	int err = raw ? hub_light_pwm(center, values) : hub_light_manual(center, color);

	if (err) {
		shell_error(shell, "LED write failed: %d", err);
	}
	return err;
}

SHELL_CMD_ARG_REGISTER(shutdown, NULL, "Switch off hub power.", cmd_shutdown, 1, 0);
SHELL_CMD_ARG_REGISTER(buttons, NULL, "Button states and press counters since boot.", cmd_buttons, 1, 0);
SHELL_CMD_ARG_REGISTER(led, NULL,
	"led [pwm] <center|bluetooth> <red> <green> <blue>. RGB: 0..255; raw PWM: 0..65535.",
	cmd_led, 5, 1);

void hub_run(void)
{
	const struct gpio_dt_spec vbus = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), vbus_gpios);
	int vbus_error = gpio_is_ready_dt(&vbus) ? gpio_pin_configure_dt(&vbus, GPIO_INPUT) : -ENODEV;
	int64_t pressed_at = -1;
	atomic_val_t center_presses = 0;
	atomic_val_t observed[ARRAY_SIZE(codes)] = {0};
	struct hub_menu menu = {0};
	bool center_was_down = false;
	int shown = -1;
	enum hub_light_menu menu_shown = HUB_LIGHT_NORMAL;
	int64_t busy_until = 0;

	for (;;) {
		int64_t now = k_uptime_get();
		native_poll();
		bool activity = false;
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
			hub_power_request_shutdown();
			shutdown();
		}
		enum host_mode current = host_interface_current();
		/* Menu entry/cancel precedes navigation. Center confirms on release,
		 * leaving its entire hold interval available for power-off. */
		const uint8_t order[] = {3, 1, 2, 0};
		for (size_t j = 0; j < ARRAY_SIZE(order); j++) {
			size_t i = order[j];
			atomic_val_t presses_now = atomic_get(&presses[i]);
			bool pressed = presses_now != observed[i];
			activity |= pressed;
			observed[i] = presses_now;
			if (pressed) {
				hub_menu_input(&menu, codes[i], true, current);
			}
			if (i == 0 && !down && (center_was_down || pressed) &&
			    hub_menu_input(&menu, codes[i], false, current)) {
				int err = menu.selected == current ? 0 : host_interface_select(menu.selected);
				if (!err) {
					menu.open = false;
					current = menu.selected;
				} else {
					busy_until = now + 150;
					if (err != -EBUSY) {
						LOG_ERR("Interface selection failed: %d", err);
					}
				}
			}
		}
		center_was_down = down;
		if (hub_power_update(vbus_error ? vbus_error : gpio_pin_get_dt(&vbus), activity, now)) {
			shutdown();
		}
		if (menu.open) {
			/* Refresh completion while presenting the selection menu. */
			uint8_t status[8];
			host_interface_status(status);
		}
		enum host_mode selected = menu.open ? menu.selected : current;
		enum hub_light_menu indicator = !menu.open ? HUB_LIGHT_NORMAL :
			now < busy_until ? HUB_LIGHT_BUSY : HUB_LIGHT_MENU;
		if (selected != shown || indicator != menu_shown) {
			int err = hub_light_select(selected, indicator, now);
			if (err) {
				LOG_ERR("Interface lighting failed: %d", err);
			}
			shown = selected;
			menu_shown = indicator;
		}
		enum host_ble_status bluetooth = HOST_BLE_OFF;
#if defined(CONFIG_BT_HCI_SETUP)
		bluetooth = host_ble_status();
#endif
		int64_t deadline = hub_light_update(now, bluetooth);
		/* Manual LED overrides must not stop tracking remotely selected modes. */
		deadline = MIN(deadline, now + 20);

		if (pressed_at >= 0) {
			deadline = MIN(deadline, pressed_at + 2000);
		}
		k_sem_take(&button_changed, deadline == INT64_MAX ? K_FOREVER :
			   K_TIMEOUT_ABS_MS(deadline));
	}
}
