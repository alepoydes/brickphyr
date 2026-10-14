// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();
static uint8_t data[600];

int app_main(void)
{
	const struct bp_record_spec spec = {.size = sizeof(data), .capacity = 3, .schema = "fixture/1"};
	if (bp_record_write(data, sizeof(data)) != -ENODATA) return 1;
	int err = bp_record_begin(&spec);
	if (err) return err;
	if (bp_record_begin(&spec) != -EALREADY) return 2;
	if (bp_record_write(data, sizeof(data) - 1) != -EMSGSIZE) return 3;
	for (unsigned int record = 0; record < 8; record++) {
		for (unsigned int i = 0; i < sizeof(data); i++) data[i] = record + i;
		err = bp_record_write(data, sizeof(data));
		if (err) return err;
	}
	/* The host stops the live writer and retrieves its retained tail. */
	for (;;) bp_sleep(1000);
}
