// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	uint32_t parameters[] = {0, 7, ~7U, 0};
	if (bp_control_start_file("missing", 7, parameters, sizeof(parameters)) != -ENOENT) return 1;
	int err = bp_control_start_file("controller", 10, parameters, sizeof(parameters));
	if (err) return err;
	struct bp_control_status status;
	do {
		err = bp_control_get(&status);
		if (err) return err;
		bp_sleep(1);
	} while (status.step.sequence < 3);
	bp_write("stored controller passed\n", 25);
	return 0;
}
