// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	uint64_t storage[(sizeof(struct bp_imu_application_state) + 15) / 8];
	void *address = bp_buttons() & 2 ? (void *)0xfffffff0U : (uint8_t *)storage + 1;
	if (bp_buttons() & 1) return bp_imu_configure(address);
	return bp_imu_get_application(address);
}
