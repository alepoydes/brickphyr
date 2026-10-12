// SPDX-License-Identifier: Apache-2.0
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include "hub.h"

LOG_MODULE_REGISTER(hub_bluetooth);
BUILD_ASSERT(DT_PROP(DT_PARENT(DT_CHOSEN(zephyr_bt_hci)), current_speed) == 115200,
	     "The initial TI script and UART must agree on baud rate");

int hub_bluetooth_power_off(void)
{
	const struct gpio_dt_spec reset =
		GPIO_DT_SPEC_GET(DT_CHOSEN(zephyr_bt_hci), reset_gpios);

	/* Also silence a controller whose HCI shutdown failed. Only reboot may
	 * release it after this terminal operation. */
	return gpio_pin_configure_dt(&reset, GPIO_OUTPUT_ACTIVE);
}

/* H:4 calls this hook before asserting/releasing the controller reset GPIO.
 * The F4 board supplies the required slow clock through TIM8/PC9. */
int bt_hci_transport_setup(const struct device *uart)
{
	const struct device *clock = DEVICE_DT_GET(DT_NODELABEL(bt_slow_clock));
	int err = device_is_ready(clock) ? clock_control_on(clock, NULL) : -ENODEV;
	if (err) {
		LOG_ERR("Bluetooth slow clock failed: %d", err);
		return err;
	}
	/* Discard UART bytes left by a previous controller session before reset,
	 * retaining the default H:4 setup's bounded drain. */
	uint8_t stale[32];
	err = uart_fifo_read(uart, stale, sizeof(stale));
	return err < 0 ? err : 0;
}
