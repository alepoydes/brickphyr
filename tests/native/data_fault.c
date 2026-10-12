// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	uint8_t value;
	if (bp_user_data_read(UINT32_MAX, &value, 1) != -EINVAL ||
	    bp_user_data_write(1, &value, SIZE_MAX) != -EINVAL) return -EINVAL;
	if (bp_buttons() & 1) return bp_user_data_read(0, NULL, 1);
	return bp_user_data_write(0, NULL, 1);
}
