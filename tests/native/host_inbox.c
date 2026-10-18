// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION_REQUIRES(0, 2048);

int app_main(void)
{
	static const struct bp_host_type types[] = {
		{.type = 1, .version = 1, .max_size = 16, .policy = BP_HOST_VALUE, .name = "parameters"},
		{.type = 2, .version = 1, .max_size = 16, .policy = BP_HOST_COMMAND, .name = "action"},
		{.type = 3, .version = 1, .max_size = 1, .policy = BP_HOST_COMMAND, .name = "release"},
	};
	int err = bp_host_open(types, 3);
	if (err) return err;
	uint8_t data[16];
	struct bp_host_message info;
	for (;;) {
		err = bp_host_read(3, data, sizeof(data), &info);
		if (err < 0) return err;
		if (err == 1) {
			if (info.size != 0) return -EINVAL;
			err = bp_host_finish(3, info.request, 0, 0);
			if (err) return err;
			break;
		}
		bp_sleep(5);
	}
	for (;;) {
		err = bp_host_read(1, data, 0, &info);
		if (err && err != -EMSGSIZE) return err < 0 ? err : -EINVAL;
		err = bp_host_read(1, data, sizeof(data), &info);
		if (err < 0) return err;
		if (err == 1) {
			if (info.size != 1) return -EINVAL;
			err = bp_host_finish(1, info.request, 0, data[0]);
			if (err) return err;
		}
		err = bp_host_read(2, data, sizeof(data), &info);
		if (err < 0) return err;
		if (err == 1) {
			if (info.size != 0) return -EINVAL;
			err = bp_host_finish(2, info.request, -ERANGE, 7);
			if (err) return err;
		}
		bp_sleep(5);
	}
}
