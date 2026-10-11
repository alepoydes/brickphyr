// SPDX-License-Identifier: Apache-2.0
#include "host_transport.h"
#include "storage.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/fff.h>

DEFINE_FFF_GLOBALS;

/* Synthetic controller; actual channel encoding is exercised in tests/hub. */
static int frame(const struct emul *emul, const struct spi_config *cfg,
		 const struct spi_buf_set *tx, const struct spi_buf_set *rx)
{
	return tx && tx->count == 1 && tx->buffers[0].len == 97 ? 0 : -EINVAL;
}

static int emul_init(const struct emul *emul, const struct device *bus) { return 0; }
static const struct spi_emul_api spi_api = {.io = frame};
EMUL_DT_DEFINE(DT_NODELABEL(hub_leds), emul_init, NULL, NULL, &spi_api, NULL);

int main(void)
{
	int result = storage_status();
	if (!result) {
		result = host_transport_init(DEVICE_DT_GET(DT_CHOSEN(zephyr_shell_uart)), 1);
	}
	if (result) {
		printk("Management initialization failed: %d\n", result);
		return result;
	}
	uint8_t state[8];
	do {
		host_interface_status(state);
		k_msleep(1);
	} while (!state[3]);
	result = host_interface_select(HOST_SHELL);
	if (result) {
		return result;
	}
	/* Native PTYs have no USB DTR events. This fixture supplies one permanent
	 * session; tests/host separately exercises the actual session transitions. */
	host_connection_changed(true);
	return 0;
}
