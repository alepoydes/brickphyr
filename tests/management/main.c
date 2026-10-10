// SPDX-License-Identifier: Apache-2.0
#include "host_transport.h"
#include "storage.h"

#include <zephyr/kernel.h>

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
