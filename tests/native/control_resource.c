// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	uint32_t parameters[] = {0, 7, ~7U, 0};
	if (bp_control_start_resource("missing", 7, parameters, sizeof(parameters)) != -ENOENT) return 1;
	if (bp_control_start_resource("../controller.elf", 17, parameters, sizeof(parameters)) != -ENOENT) return 2;
	int err = bp_control_start_resource("controller.elf", 14, parameters, sizeof(parameters));
	if (err) return err;
	struct bp_control_status status;
	do {
		err = bp_control_get(&status);
		if (err) return err;
		bp_sleep(1);
	} while (status.step.sequence < 3);
	bp_write("package controller passed\n", 26);
	for (;;) bp_sleep(1000);
}
