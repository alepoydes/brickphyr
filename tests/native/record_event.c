// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	const struct bp_record_spec spec = {.size = sizeof(uint32_t), .capacity = 4,
		.schema = "fixture.event/1"};
	int err = bp_record_begin(&spec);
	if (err) return err;
	for (uint32_t value = 0; value < 10; value++) {
		err = bp_record_write(&value, sizeof(value));
		if (err) return err;
		if (value == 7) {
			if (bp_record_trigger(4) != -EINVAL) return 1;
			err = bp_record_trigger(2);
			if (err) return err;
			if (bp_record_trigger(0) != -EALREADY) return 2;
		}
	}
	uint32_t ignored = 99;
	if (bp_record_write(&ignored, sizeof(ignored)) != -ENOSPC) return 3;
	/* A completed capture must not stop the autonomous application. */
	for (;;) bp_sleep(1000);
}
