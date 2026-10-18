// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_BATTERY, 2048);

/* Passive fixture: targets are synthetic; no actuator service is called. */
int app_main(void)
{
	struct bp_control_frame frame;
	int err = bp_control_frame_get(0, &frame);
	if (err) return err;
	uint8_t base[BP_CAPTURE_CONFIG_SIZE] = {0};
	sys_put_le32(1U << 2, base);
	sys_put_le32(15, base + 4);
	sys_put_le32(frame.capture_resolution_us, base + 12);
	base[16 + 2] = 1;
	static const struct bp_capture_app_field fields[] = {
		{"target_speed_mm_s", 'h'}, {"target_turn_mdps", 'i'},
	};
	uint8_t config[BP_CAPTURE_APP_MAX_CONFIG_SIZE];
	size_t app_bytes;
	int config_size = bp_capture_app_config(base, fields, 2, config, sizeof(config),
		&app_bytes);
	if (config_size < 0) return config_size;
	uint8_t row[64 + 6];
	if (app_bytes != 6) return -EINVAL;
	const struct bp_record_spec spec = {.size = sizeof(row), .capacity = 80,
		.schema = "brickphyr.capture+app/1"};
	err = bp_record_begin_segments(&spec, config, config_size, true);
	if (err) return err;
	for (unsigned int written = 0; written < 100;) {
		struct bp_control_step step;
		err = bp_control_wait(15000, &step, NULL, 0);
		if (err) return err;
		err = bp_control_frame_get(0, &frame);
		if (err) return err;
		err = bp_capture_pack_frame(base, sizeof(base), &step, &frame,
			row, sizeof(row) - app_bytes);
		if (err != sizeof(row) - app_bytes) return err < 0 ? err : -EMSGSIZE;
		int16_t speed = written < 50 ? -120 : 80;
		int32_t turn = written < 50 ? 900 : -450;
		sys_put_le16((uint16_t)speed, row + err);
		sys_put_le32((uint32_t)turn, row + err + 2);
		err = bp_record_write(row, sizeof(row));
		if (!err) written++;
		else if (err != -ENOSPC) return err;
	}
	return 0;
}
