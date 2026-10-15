// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>
#include <stdbool.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU | BP_CAP_SETTINGS, 2048);

#define FIRST_CORRECTION 361000
#define SECOND_CORRECTION 362000

static int select_correction(int32_t correction)
{
	struct bp_imu_command command = {
		.flags = BP_IMU_CALIBRATION,
		.settings_mask = 1U << BP_IMU_HEADING_CORRECTION,
	};
	command.settings.values[BP_IMU_HEADING_CORRECTION][0] = correction;
	int err = bp_imu_configure(&command);
	if (err) return err;
	for (int attempt = 0; attempt < 2000; attempt++) {
		struct bp_imu_application_state state;
		err = bp_imu_get_application(&state);
		if (err) return err;
		if (!state.pending) {
			if (state.command_error) return state.command_error;
			return state.settings.values[BP_IMU_HEADING_CORRECTION][0] == correction ?
				0 : -EBADMSG;
		}
		err = bp_sleep(1);
		if (err) return err;
	}
	return -ETIMEDOUT;
}

int app_main(void)
{
	struct bp_imu_application_state state;
	int err = bp_imu_get_application(&state);
	if (err) return err;
	bool second = state.settings.values[BP_IMU_HEADING_CORRECTION][0] == FIRST_CORRECTION;
	err = select_correction(second ? SECOND_CORRECTION : FIRST_CORRECTION);
	if (err) return err;
	if (!second) return bp_settings_save();
	/* The accepted save owns no ELF memory and must outlive this return. */
	int request = bp_settings_save_start();
	return request > 0 ? 73 : request;
}
