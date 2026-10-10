// SPDX-License-Identifier: Apache-2.0
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hub_bluetooth);

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
