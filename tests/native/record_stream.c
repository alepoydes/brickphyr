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

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU | BP_CAP_BATTERY | BP_CAP_PORTS, 2048);

/* Physical BLE transfer workload. Port state is read only; no motor API is called. */
static struct bp_control_frame frame;
static uint8_t row[240];

int app_main(void)
{
	struct { uint32_t samples, token; } parameters = {100, 0};
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
	err = RECORD_SEGMENTS ? bp_record_begin_segments(&spec, config, sizeof(config), true) :
		RECORD_RING ? bp_control_record_begin(&spec, config, sizeof(config)) :
		bp_record_begin_paused(&spec, config, sizeof(config), true);
	if (err) return err;
	unsigned int limit = 100;
	for (unsigned int written = 0; written < limit;) {
		struct bp_control_step step;
		err = bp_control_wait(15000, &step, &parameters, sizeof(parameters));
		if (err) return err;
		if (step.parameter_size && step.parameter_size != sizeof(parameters)) return -EMSGSIZE;
		if (step.sequence == 1 && step.parameter_size) {
			if (parameters.samples < 1 || parameters.samples > 4000) return -EINVAL;
			limit = parameters.samples;
		}
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
