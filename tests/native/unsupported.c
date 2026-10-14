// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	uint8_t value;
	const void *unreadable = (const void *)(uintptr_t)0xfffffff0U;
	void *unwritable = (void *)(uintptr_t)0xfffffff0U;

	/* Scalar bounds still take precedence over an unavailable backend. */
	if (bp_user_data_read(UINT32_MAX, &value, 1) != -EINVAL ||
	    bp_user_data_write(1, &value, SIZE_MAX) != -EINVAL) return -EINVAL;

	/* Disabled services must not inspect or copy these user pointers. */
	if (bp_user_data_read(0, unwritable, 1) != -ENOTSUP ||
	    bp_user_data_write(0, unreadable, 1) != -ENOTSUP ||
	    bp_imu_get(unwritable) != -ENOTSUP ||
	    bp_radio_observe(unreadable, 1, unwritable) != -ENOTSUP ||
	    bp_ble_peer_open(unreadable, unwritable) != -ENOTSUP ||
	    bp_uart_write(0, 0, unreadable, 1, unwritable) != -ENOTSUP ||
	    bp_motor_control(0, unreadable) != -ENOTSUP) return -EINVAL;

	return bp_uart_read(0, 0, unwritable, 0) == 0 ? 0 : -EINVAL;
}
