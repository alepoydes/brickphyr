// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>

#ifndef RECORD_RING
#define RECORD_RING 0
#endif
#ifndef RECORD_SEGMENTS
#define RECORD_SEGMENTS 0
#endif

BRICKPHYR_APPLICATION();

int app_main(void)
{
	struct bp_control_frame frame;
	int err = bp_control_frame_get(0, &frame);
	if (err) return err;
	uint8_t config[BP_CAPTURE_CONFIG_SIZE] = {0};
	sys_put_le32(1U << 2, config);
	sys_put_le32(10, config + 4);
	sys_put_le32(frame.capture_resolution_us, config + 12);
	config[16 + 2] = 1; /* battery voltage */
	const struct bp_record_spec spec = {.size = 64, .capacity = 3,
		.schema = "brickphyr.capture/2"};
	err = RECORD_SEGMENTS ? bp_record_begin_segments(&spec, config, sizeof(config), true) :
		RECORD_RING ? bp_control_record_begin(&spec, config, sizeof(config)) :
		bp_record_begin_paused(&spec, config, sizeof(config), true);
	if (err) return err;
	for (unsigned int written = 0; written < (RECORD_RING ? 20 : 10);) {
		struct bp_control_step step;
		err = bp_control_wait(10000, &step, NULL, 0);
		if (err) return err;
		err = bp_control_frame_get(0, &frame);
		if (err) return err;
		uint8_t row[64];
		err = bp_capture_pack_frame(config, sizeof(config), &step, &frame, row, sizeof(row));
		if (err != sizeof(row)) return err < 0 ? err : -EMSGSIZE;
		err = bp_record_write(row, sizeof(row));
		if (!err) written++;
		else if (err != -ENOSPC) return err;
	}
	return 0;
}
