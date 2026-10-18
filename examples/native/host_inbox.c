// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION_REQUIRES(0, 2048);

static const struct bp_host_type types[] = {
	{.type = 1, .version = 1, .max_size = 1024, .policy = BP_HOST_VALUE, .name = "parameters"},
	{.type = 2, .version = 1, .max_size = 1, .policy = BP_HOST_COMMAND, .name = "ping"},
};
static uint8_t data[1024];

int app_main(void)
{
	int err = bp_host_open(types, 2);
	if (err) return err;
	struct bp_host_message info;
	for (;;) {
		err = bp_host_read(1, data, sizeof(data), &info);
		if (err < 0) return err;
		if (err) {
			/* This example's format starts with a little-endian revision. A
			 * controller would validate the rest before bp_control_update(). */
			uint32_t revision = info.size >= 4 ?
				(uint32_t)data[0] | (uint32_t)data[1] << 8 |
				(uint32_t)data[2] << 16 | (uint32_t)data[3] << 24 : 0;
			err = bp_host_finish(1, info.request, info.size >= 4 ? 0 : -EINVAL, revision);
			if (err) return err;
		}
		err = bp_host_read(2, data, sizeof(data), &info);
		if (err < 0) return err;
		if (err) {
			err = bp_host_finish(2, info.request, info.size ? -EINVAL : 0, 1);
			if (err) return err;
		}
		err = bp_sleep(10);
		if (err) return err;
	}
}
