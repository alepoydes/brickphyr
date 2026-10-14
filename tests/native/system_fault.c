// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>
#include <string.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	struct bp_system_info info;
	if (bp_system_get(&info) || strcmp(info.name, "Brickphyr") || info.program_id != -1 ||
	    info.program_start_type != 3 || bp_set_stop_buttons(16) != -EINVAL ||
	    bp_set_stop_buttons(0)) return -EINVAL;
	if (bp_buttons() & 1) {
		/* Readable .rodata is not an output buffer. IMU is disabled in this
		 * fixture, so no backend write can accidentally supply the fault. */
		static const struct bp_imu_sample readonly;
		return bp_imu_get((struct bp_imu_sample *)&readonly);
	}
	uint32_t unaligned[sizeof(info) / sizeof(uint32_t) + 2];
	return bp_system_get((struct bp_system_info *)((uint8_t *)unaligned + 1));
}
