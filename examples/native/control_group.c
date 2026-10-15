// SPDX-License-Identifier: Apache-2.0
#include "control_record.h"
#include <errno.h>
#include <stdbool.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_PORTS | BP_CAP_PORT_UART | BP_CAP_MOTORS, 2048);

/* brickphyr.control-group/2, little endian. Each observation retains the exact
 * input used for this one command; later driver output is stored separately.
 * Request IDs distinguish accepted, applied and expired commands. */
struct group_record {
	struct control_record input;
	int64_t observed_ms, command_ms, deadline_ms;
	uint32_t phase, request, port_mask, coast_mask;
	int32_t issue_error, command_duty[2];
	uint32_t command_generation[2];
	int32_t group_read_error;
	int64_t group_requested_ms, group_completed_ms, group_deadline_ms;
	uint32_t group_request, group_mask, applied_mask;
	int32_t group_error, cleanup_error;
	uint32_t group_flags;
	uint32_t group_cleanup_mask, group_timestamp_resolution_us;
	struct {
		uint32_t group_generation, group_request;
		int32_t group_error, cleanup_error;
		int64_t applied_ms;
		uint32_t generation, request, completed, flags;
		int32_t duty_permille, error;
		int64_t group_applied_us, group_cleanup_us;
		int32_t group_duty_permille;
		uint8_t group_mode, reserved[3];
	} ports[2];
};
_Static_assert(sizeof(struct group_record) == 440, "Group record layout");

enum phase { COMMAND, APPLIED, ENDED, WAIT_FAILED };
static struct bp_control_frame frame;
static struct group_record record;

static int save_observation(enum phase phase, int64_t observed_ms,
	const struct bp_motor_group_state *state, int read_error)
{
	/* These two snapshots are not an atomic latch. Compare per-port completion
	 * IDs with the group's original IDs before attributing an output to it. */
	int err = bp_control_frame_get(record.port_mask, &frame);
	if (err) return err;
	record.observed_ms = observed_ms;
	record.phase = phase;
	record.group_read_error = read_error;
	record.group_requested_ms = state->requested_ms;
	record.group_completed_ms = state->completed_ms;
	record.group_deadline_ms = state->deadline_ms;
	record.group_request = state->request;
	record.group_mask = state->port_mask;
	record.applied_mask = state->applied_mask;
	record.group_error = state->error;
	record.cleanup_error = state->cleanup_error;
	record.group_flags = state->pending | ((uint32_t)state->active << 1);
	record.group_cleanup_mask = state->cleanup_mask;
	record.group_timestamp_resolution_us = state->timestamp_resolution_us;
	for (unsigned int port = 0; port < 2; port++) {
		const struct bp_control_port *p = &frame.ports[port];
		record.ports[port].group_generation = state->ports[port].generation;
		record.ports[port].group_request = state->ports[port].request;
		record.ports[port].group_error = state->ports[port].error;
		record.ports[port].cleanup_error = state->ports[port].cleanup_error;
		record.ports[port].applied_ms = p->applied_ms;
		record.ports[port].generation = p->output_generation;
		record.ports[port].request = p->request;
		record.ports[port].completed = p->completed;
		record.ports[port].flags = p->mode | ((uint32_t)p->valid << 8) |
			((uint32_t)p->pending << 9) | ((uint32_t)p->owner << 16);
		record.ports[port].duty_permille = p->duty_permille;
		record.ports[port].error = p->output_error;
		record.ports[port].group_applied_us = state->ports[port].applied_us;
		record.ports[port].group_cleanup_us = state->ports[port].cleanup_us;
		record.ports[port].group_duty_permille = state->ports[port].duty_permille;
		record.ports[port].group_mode = state->ports[port].mode;
	}
	return bp_record_write(&record, sizeof(record));
}

int app_main(void)
{
	/* The default target moves A/B encoder motors. Clear both mechanisms.
	 * control_group_coast builds this same path with passive outputs only. */
	const uint32_t mask = (1U << 0) | (1U << 1);
	const struct bp_record_spec spec = {
		.size = sizeof(record), .capacity = 4, .schema = "brickphyr.control-group/2",
	};
	int err = bp_record_begin(&spec);
	if (err) return err;
	err = bp_control_frame_get(mask, &frame);
	if (err) return err;
	control_record_fill(&record.input, &frame);
	record.command_ms = bp_time_ms();
	struct bp_motor_group_command command = {
		.port_mask = mask, .deadline_ms = record.command_ms + 100,
	};
#ifdef BP_GROUP_COAST
	command.coast_mask = mask;
	command.deadline_ms = 0;
#endif
	record.port_mask = mask;
	record.coast_mask = command.coast_mask;
	record.deadline_ms = command.deadline_ms;
	for (uint32_t port = 0; port < 2; port++) {
		if (!record.issue_error) record.issue_error = frame.ports[port].error;
		command.ports[port].generation = record.command_generation[port] = frame.ports[port].generation;
		command.ports[port].duty_permille = record.command_duty[port] =
			command.coast_mask ? 0 : port ? -200 : 200;
	}
	if (!record.issue_error) record.issue_error = bp_motor_group_set(&command, &record.request);
	struct bp_motor_group_state state = {0};
	int64_t observed_ms = bp_time_ms();
	int read_error = bp_motor_group_get(&state);
	err = save_observation(COMMAND, observed_ms, &state, read_error);
	if (record.issue_error) return record.issue_error;
	if (read_error) return read_error;
	if (err) return err;
	bool applied = false;
	for (;;) {
		if (state.request != record.request) return -ESTALE;
		if (!state.pending && !state.active) {
			err = save_observation(ENDED, observed_ms, &state, 0);
			/* This demonstration intentionally lets its lease expire. */
			int expected = command.coast_mask ? 0 : -ETIMEDOUT;
			if (state.error != expected) return state.error ? state.error : -EIO;
			return state.cleanup_error ? state.cleanup_error : err;
		}
		if (!state.pending && !applied) {
			err = save_observation(APPLIED, observed_ms, &state, 0);
			if (err) return err;
			applied = true;
		}
		if (bp_time_ms() > record.command_ms + 200) {
			(void)save_observation(WAIT_FAILED, observed_ms, &state, 0);
			return -ETIMEDOUT;
		}
		err = bp_sleep(1);
		if (err) return err;
		observed_ms = bp_time_ms();
		read_error = bp_motor_group_get(&state);
		if (read_error) {
			(void)save_observation(WAIT_FAILED, observed_ms, &state, read_error);
			return read_error;
		}
	}
}
