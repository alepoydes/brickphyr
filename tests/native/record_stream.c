// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU | BP_CAP_BATTERY | BP_CAP_PORTS, 2048);

/* Physical BLE transfer workload. Port state is read only; no motor API is called.
 * A full buffer skips recording, not control, until acknowledged by the host. */
static struct bp_control_frame frame;
static uint8_t row[240];

int app_main(void)
{
	int err = bp_control_frame_get(3, &frame);
	if (err) return err;
	uint8_t config[BP_CAPTURE_CONFIG_SIZE] = {0};
	sys_put_le32(125, config); /* IMU, battery, A/B encoders and outputs. */
	sys_put_le32(15, config + 4);
	sys_put_le32(frame.capture_resolution_us, config + 12);
	config[16] = 63; /* Six raw IMU values. */
	config[18] = config[19] = config[20] = config[21] = config[22] = 1;
	const struct bp_record_spec spec = {.size = sizeof(row), .capacity = 80,
		.schema = "brickphyr.capture/2"};
	err = bp_record_begin_paused(&spec, config, sizeof(config), true);
	if (err) return err;
	for (unsigned int written = 0; written < 100;) {
		struct bp_control_step step;
		err = bp_control_wait(15000, &step, NULL, 0);
		if (err) return err;
		err = bp_control_frame_get(3, &frame);
		if (err) return err;
		err = bp_capture_pack_frame(config, sizeof(config), &step, &frame, row, sizeof(row));
		if (err != sizeof(row)) return err < 0 ? err : -EMSGSIZE;
		err = bp_record_write(row, sizeof(row));
		if (!err) written++;
		else if (err != -ENOSPC) return err;
	}
	return 0;
}
