// SPDX-License-Identifier: Apache-2.0
#include "application.h"
#include "host_transport.h"
#include "hub_light.h"
#include "hub_charger.h"
#include "hub_power.h"
#include "../ports/wiring.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/fff.h>
#include <zephyr/shell/shell.h>

DEFINE_FFF_GLOBALS;

static uint8_t led_frame[97];
static struct k_spinlock frame_lock;
static atomic_t led_error;

static int receive_frame(const struct emul *emul, const struct spi_config *cfg,
			 const struct spi_buf_set *tx, const struct spi_buf_set *rx)
{
	if (atomic_get(&led_error)) {
		return -EIO;
	}
	if (tx->count != 1 || tx->buffers[0].len != sizeof(led_frame)) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&frame_lock);
	memcpy(led_frame, tx->buffers[0].buf, sizeof(led_frame));
	k_spin_unlock(&frame_lock, key);
	return 0;
}

static int emul_init(const struct emul *emul, const struct device *bus) { return 0; }
static const struct spi_emul_api spi_api = {.io = receive_frame};
EMUL_DT_DEFINE(DT_NODELABEL(hub_leds), emul_init, NULL, NULL, &spi_api, NULL);

/* Observe the synthetic SPI peer, independently of Python/service snapshots. */
static int frame_snapshot(const struct shell *shell, size_t argc, char **argv)
{
	uint8_t frame[sizeof(led_frame)];
	k_spinlock_key_t key = k_spin_lock(&frame_lock);
	memcpy(frame, led_frame, sizeof(frame));
	k_spin_unlock(&frame_lock, key);
	char text[sizeof(frame) * 2 + 1];
	bin2hex(frame, sizeof(frame), text, sizeof(text));
	shell_print(shell, "SPI: %s", text);
	return 0;
}
SHELL_CMD_ARG_REGISTER(led_frame, NULL, "Synthetic SPI frame.", frame_snapshot, 1, 0);

static int frame_failure(const struct shell *shell, size_t argc, char **argv)
{
	int err = 0;
	unsigned long value = shell_strtoul(argv[1], 10, &err);
	if (err || value > 1) {
		return -EINVAL;
	}
	atomic_set(&led_error, value);
	return 0;
}
SHELL_CMD_ARG_REGISTER(led_error, NULL, "Synthetic SPI failure: 0|1.", frame_failure, 2, 0);

static atomic_t pressed_buttons = ATOMIC_INIT(5);
uint32_t hub_buttons(void) { return atomic_get(&pressed_buttons); }
static int button_input(const struct shell *shell, size_t argc, char **argv)
{
	int err = 0;
	unsigned long value = shell_strtoul(argv[1], 10, &err);
	if (err || value > 15) return -EINVAL;
	atomic_set(&pressed_buttons, value);
	application_buttons(value);
	return 0;
}
SHELL_CMD_ARG_REGISTER(button_input, NULL, "Synthetic button mask, 0..15.", button_input, 2, 0);
int storage_status(void) { return -ENOTSUP; }

/* QEMU UART has no DTR. Exercise the production notification explicitly. */
static int close_session(const struct shell *shell, size_t argc, char **argv)
{
	application_session_closed();
	shell_print(shell, "Synthetic session boundary");
	return 0;
}
SHELL_CMD_ARG_REGISTER(session_close, NULL, "Synthetic disconnect notification.", close_session, 1, 0);

#ifdef CONFIG_BRICKPHYR_PYTHON
static int hold_heap(const struct shell *shell, size_t argc, char **argv)
{
	static void *held;
	int err = 0;
	unsigned long size = shell_strtoul(argv[1], 10, &err);
	if (err) return err;
	k_free(held);
	held = size ? k_malloc(size) : NULL;
	if (size && !held) return -ENOMEM;
	shell_print(shell, "Heap held: %lu", size);
	return 0;
}
SHELL_CMD_ARG_REGISTER(heap_hold, NULL, "Synthetic heap pressure in bytes.", hold_heap, 2, 0);
#endif

#ifndef CONFIG_SHELL_BACKEND_SERIAL
static atomic_t detach_requested;
static int detach(const struct shell *shell, size_t argc, char **argv)
{
	atomic_set(&detach_requested, 1);
	shell_print(shell, "Synthetic DTR cycle scheduled");
	return 0;
}
SHELL_CMD_ARG_REGISTER(detach, NULL, "Schedule a synthetic DTR cycle in 1.2 s.", detach, 1, 0);
#endif

int main(void)
{
#ifndef CONFIG_BRICKPHYR_PORT_UART
	/* Passive-only profile: exercise the remaining bindings without UART. */
	pup_test_circuits[0].kind = TOUCH_SWITCH;
	pup_test_circuits[1].kind = MEDIUM_MOTOR;
#endif
	/* Synthetic configured host and stable charge indication. The production
	 * service acquires these ADC/ladder samples in the normal hub loop below. */
	const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc1));
	__ASSERT_NO_MSG(!adc_emul_const_raw_value_set(adc, 3, 1000));
	__ASSERT_NO_MSG(!adc_emul_const_raw_value_set(adc, 14, 3600));
	__ASSERT_NO_MSG(!adc_emul_const_raw_value_set(adc, 1, 4095));
	hub_charger_usb(HUB_CHARGER_USB_CONFIGURED);
#ifndef CONFIG_SHELL_BACKEND_SERIAL
	/* Actual shared host dispatcher over QEMU's UART. DTR is synthetic. */
	int err = host_transport_init(DEVICE_DT_GET(DT_CHOSEN(zephyr_console)), 0);
	__ASSERT_NO_MSG(!err);
	uint8_t state[8];
	do {
		k_msleep(1);
		host_interface_status(state);
	} while (!state[3]);
	__ASSERT_NO_MSG(!host_interface_select(HOST_SHELL));
	host_connection_changed(true);
	int64_t detach_at = 0;
#endif
	for (;;) {
		application_poll();
		application_buttons(hub_buttons());
		struct hub_power_state power;
		hub_power_get(&power);
		if (power.shutting_down) application_shutdown();
		hub_light_update(k_uptime_get(), HOST_BLE_OFF);
		hub_charger_update(1, k_uptime_get());
#ifndef CONFIG_SHELL_BACKEND_SERIAL
		if (atomic_cas(&detach_requested, 1, 0)) {
			detach_at = k_uptime_get() + 1200;
		}
		if (detach_at && k_uptime_get() >= detach_at) {
			detach_at = 0;
			host_connection_changed(false);
			do {
				k_msleep(1);
				host_interface_status(state);
			} while (!state[3]);
			host_connection_changed(true);
		}
#endif
		k_msleep(20);
	}
}
