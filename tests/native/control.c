// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>
#include <stdbool.h>

BRICKPHYR_APPLICATION();
static uint32_t private_value;

int app_main(void)
{
	struct parameters { uint32_t mode, value, inverse, address; } parameters;
	struct record {
		struct bp_control_step step;
		uint32_t value;
		float accumulator;
		uint32_t address, reserved;
	} record = {0};
	const struct bp_record_spec spec = {
		.size = sizeof(record), .capacity = 16, .schema = "fixture.control/1",
	};
	int err = bp_record_begin(&spec);
	if (err) return err;
	bool delayed = false;
	for (;;) {
		err = bp_control_wait(10000, &record.step, &parameters, sizeof(parameters));
		if (err) return err;
		if (record.step.parameter_size != sizeof(parameters) || parameters.inverse != ~parameters.value) return -EBADMSG;
		record.value = parameters.value;
		record.accumulator += 0.25f;
		record.address = (uintptr_t)&private_value;
		err = bp_record_write(&record, sizeof(record));
		if (err) return err;
		switch (parameters.mode) {
		case 1: return 77;
		case 2: return *(volatile uint32_t *)(uintptr_t)parameters.address;
		case 3: for (;;) { }
		case 4:
			if (!delayed) { bp_sleep(35); delayed = true; }
			break;
		}
	}
}
