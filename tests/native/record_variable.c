// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	const struct bp_record_spec spec = {.size = 5, .capacity = 3,
		.schema = "variable-probe/1"};
	int err = bp_record_begin_variable_paused(&spec, NULL, 0);
	if (err) return err;
	for (uint8_t value = 0; value < 10;) {
		uint8_t data[5] = {value, 1, 2, 3, 4};
		err = bp_record_write(data, 1 + value % 5);
		if (!err) value++;
		else if (err != -ENOSPC) return err;
		bp_sleep(1);
	}
	return 0;
}
