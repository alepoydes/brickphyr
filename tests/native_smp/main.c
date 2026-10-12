// SPDX-License-Identifier: Apache-2.0
#include "application.h"
#include "host_transport.h"
#include "hub_light.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/emul.h>
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

uint32_t hub_buttons(void) { return 5; } /* Synthetic buttons. */
int storage_status(void) { return -ENOTSUP; }

/* QEMU UART has no DTR. Exercise the production notification explicitly. */
static int close_session(const struct shell *shell, size_t argc, char **argv)
{
	application_session_closed();
	shell_print(shell, "Synthetic session boundary");
	return 0;
}
SHELL_CMD_ARG_REGISTER(session_close, NULL, "Synthetic disconnect notification.", close_session, 1, 0);

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
		hub_light_update(k_uptime_get(), HOST_BLE_OFF);
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
