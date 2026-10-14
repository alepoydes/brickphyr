// SPDX-License-Identifier: Apache-2.0
#include "control_record.h"
#include <errno.h>
#include <stdbool.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_PORTS | BP_CAP_PORT_UART | BP_CAP_MOTORS, 2048);

/* This is an output-timing example, not a feedback controller. The default
 * target drives suspended A/B wheels at opposite 20% duty, then coasts.
 * The _coast target runs the same path without energizing either output. */
static struct bp_control_frame frame;
static bool recording = true;
static struct {
	struct control_record input;
	int64_t command_ms, deadline_ms;
	uint32_t phase, port_mask, coast_mask, request, previous_sequence;
	int32_t issue_error, duty[2];
	uint32_t generation[2];
	int64_t previous_requested_ms, previous_completed_ms, previous_deadline_ms;
	uint32_t previous_request, previous_mask, previous_applied_mask;
	int32_t previous_error, previous_cleanup_error;
	uint32_t previous_flags;
	struct {
		uint32_t generation, request;
		int32_t error, cleanup_error;
	} previous_ports[2];
} record;
_Static_assert(sizeof(record) == 312, "Control actuation record layout");

static int save(const struct bp_control_motor_result *result)
{
	if (!recording) return 0;
	record.request = result->request;
	record.previous_sequence = result->previous_sequence;
	const struct bp_motor_group_state *previous = &result->previous;
	record.previous_requested_ms = previous->requested_ms;
	record.previous_completed_ms = previous->completed_ms;
	record.previous_deadline_ms = previous->deadline_ms;
	record.previous_request = previous->request;
	record.previous_mask = previous->port_mask;
	record.previous_applied_mask = previous->applied_mask;
	record.previous_error = previous->error;
	record.previous_cleanup_error = previous->cleanup_error;
	record.previous_flags = previous->pending | ((uint32_t)previous->active << 1);
	for (uint32_t port = 0; port < 2; port++) {
		record.previous_ports[port].generation = previous->ports[port].generation;
		record.previous_ports[port].request = previous->ports[port].request;
		record.previous_ports[port].error = previous->ports[port].error;
		record.previous_ports[port].cleanup_error = previous->ports[port].cleanup_error;
	}
	int err = bp_record_write(&record, sizeof(record));
	if (err == -ENOSPC) {
		recording = false;
		return 0;
	}
	return err;
}

int app_main(void)
{
	const struct { uint32_t period_us, steps; } config = {20000, 16};
	const struct bp_record_spec spec = {
		.size = sizeof(record), .capacity = 32, .schema = "brickphyr.control-actuation/1",
	};
	int err = bp_control_record_begin(&spec, &config, sizeof(config));
	if (err) return err;
	struct bp_control_step step;
	struct bp_control_motor_result result;
	for (uint32_t i = 0; i < config.steps; i++) {
		err = bp_control_wait(config.period_us, &step, NULL, 0);
		if (err) return err;
		err = bp_control_frame_get(3, &frame);
		if (err) return err;
		control_record_fill(&record.input, &frame);
		record.command_ms = bp_time_ms();
		struct bp_motor_group_command command = {
			.port_mask = 3, .deadline_ms = step.scheduled_us / 1000 + 40,
		};
		if (i + 1 == config.steps) command.coast_mask = 3;
#ifdef BP_GROUP_COAST
		command.coast_mask = 3;
#endif
		if (command.coast_mask) command.deadline_ms = 0;
		record.port_mask = command.port_mask;
		record.coast_mask = command.coast_mask;
		record.deadline_ms = command.deadline_ms;
		for (uint32_t port = 0; port < 2; port++) {
			command.ports[port].generation = record.generation[port] = frame.ports[port].generation;
			command.ports[port].duty_permille = record.duty[port] =
				command.coast_mask ? 0 : port ? -200 : 200;
		}
		record.issue_error = bp_control_motor_group_set(&command, &result);
		err = save(&result);
		/* Preserve a command failure even when the diagnostic buffer is frozen. */
		if (result.previous_sequence && result.previous.error) return result.previous.error;
		if (result.previous_sequence && result.previous.cleanup_error) return result.previous.cleanup_error;
		if (record.issue_error) return record.issue_error;
		if (err) return err;
	}
	/* No further replacement can hide the final coast. Its original input and
	 * step remain in the record; phase 1 reports completion, not another issue. */
	uint32_t final_request = result.request;
	result.request = 0;
	result.previous_sequence = step.sequence;
	int64_t limit = bp_time_ms() + 100;
	for (;;) {
		err = bp_motor_group_get(&result.previous);
		if (err) return err;
		if (result.previous.request != final_request) return -ESTALE;
		if (!result.previous.pending && !result.previous.active) break;
		if (bp_time_ms() >= limit) return -ETIMEDOUT;
		err = bp_sleep(1);
		if (err) return err;
	}
	record.phase = 1;
	err = save(&result);
	return result.previous.error ? result.previous.error :
		result.previous.cleanup_error ? result.previous.cleanup_error : err;
}
