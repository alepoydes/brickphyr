// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>
#include <stdint.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	const struct bp_record_spec spec = {.size = sizeof(uint32_t), .capacity = 3,
		.schema = "paused-probe/1"};
	int err = bp_record_begin_paused(&spec, NULL, 0, false);
	if (err) return err;
	for (uint32_t value = 0; value < 10;) {
		err = bp_record_write(&value, sizeof(value));
		if (!err) value++;
		else if (err != -ENOSPC) return err;
		bp_sleep(1);
	}
	return 0;
}
