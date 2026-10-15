// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();
static const uint8_t controller_image[] = {
#include "background_controller.inc"
};

int app_main(void)
{
	struct parameters { uint32_t mode, value, inverse, address; } parameters = {0, 7, ~7U, 0};
	struct record {
		struct bp_control_step step;
		uint32_t value;
		float accumulator;
		uint32_t address, reserved;
	} record;
	if (bp_control_start("invalid", 7, &parameters, sizeof(parameters)) >= 0) return 1;
	int err = bp_control_start(controller_image, sizeof(controller_image), &parameters, sizeof(parameters));
	if (err) return err;
	if (bp_control_start(controller_image, sizeof(controller_image), &parameters, sizeof(parameters)) != -EBUSY) return 2;
	struct bp_control_step step;
	if (bp_control_wait(10000, &step, NULL, 0) != -EPERM || bp_record_write(NULL, 0) != -EPERM) return 3;
	struct bp_record_info info;
	struct bp_record_request request = {
		.data = &record, .capacity = sizeof(record),
		.parameters = &parameters, .parameters_capacity = sizeof(parameters),
	};
	uint32_t observed = 0;
#if BACKGROUND_MODE == 0
	uint32_t revision = 1;
#endif
	float accumulator = 0;
	for (;;) {
		/* Busy native numerical work must not prevent periodic control or stop. */
		for (unsigned int i = 0; i < 4096; i++) accumulator += 0.25f;
		if (accumulator == 1048576.0f) accumulator = 0;
		if (accumulator < 0 || accumulator >= 1048576.0f) return 4;
		err = bp_record_get(&info);
		if (err == -ENODATA) continue;
		if (err) return err;
		request.generation = info.generation;
		if (request.index < info.first) request.index = info.first;
		err = bp_record_readinto(&request, &info, &step);
		if (!err || err == -EOVERFLOW) continue;
		if (err < 0) return err;
		if (err != sizeof(record) || step.sequence != record.step.sequence ||
		    step.parameter_revision != record.step.parameter_revision ||
		    record.value != parameters.value || parameters.inverse != ~parameters.value ||
		    record.accumulator != step.sequence * 0.25f) return 5;
		request.index++;
		observed++;
		if (observed < 4) continue;
#if BACKGROUND_MODE == 1
		/* Primary and controller must not map each other's private BSS. */
		return *(volatile uint32_t *)(uintptr_t)record.address;
#elif BACKGROUND_MODE == 2
		continue;
#else
		if (revision == 1) {
			parameters.value = 0x12345678;
			parameters.inverse = ~parameters.value;
			err = bp_control_update(&parameters, sizeof(parameters));
			if (err != 2) return 6;
			revision = err;
		} else if (step.parameter_revision == revision) {
			struct bp_control_status state;
			if (bp_control_get(&state) || state.revision != revision ||
			    !state.loaded_bytes || !state.loader_bytes) return 7;
			bp_write("native background complete\n", 27);
			return 0;
		}
#endif
	}
}
