// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <brickphyr.h>

/* brickphyr.control-input/1: little endian, SDK units, no implicit padding.
 * Shared by these examples, not a universal robot record format. */
struct control_record {
	int64_t capture_us[2], imu_us[2], port_ms[2], angle_mdeg[2];
	int32_t acceleration[3], angular_velocity[3], errors[4];
	uint32_t imu_epoch, imu_sequence, generation[2], battery_mv, resolution_us;
	int64_t applied_ms[2];
	int32_t duty_permille[2], output_errors[2];
	uint32_t completed[2], output_generation[2];
};
_Static_assert(sizeof(struct control_record) == 176, "Control record layout");

static void control_record_fill(struct control_record *record, const struct bp_control_frame *frame)
{
	*record = (struct control_record){
		.capture_us = {frame->capture_start_us, frame->capture_end_us},
		.imu_us = {frame->imu.raw.sample_us, frame->imu.raw.read_us},
		.errors = {frame->imu.error, frame->ports[0].error, frame->ports[1].error, frame->battery.error},
		.imu_epoch = frame->imu.epoch, .imu_sequence = frame->imu.raw.sequence,
		.battery_mv = frame->battery.voltage_mv, .resolution_us = frame->capture_resolution_us,
	};
	for (unsigned int axis = 0; axis < 3; axis++) {
		record->acceleration[axis] = frame->imu.acceleration[axis];
		record->angular_velocity[axis] = frame->imu.angular_velocity[axis];
	}
	for (unsigned int port = 0; port < 2; port++) {
		const struct bp_control_port *p = &frame->ports[port];
		record->port_ms[port] = p->sample_ms;
		record->angle_mdeg[port] = p->angle_mdeg;
		record->generation[port] = p->generation;
		record->applied_ms[port] = p->applied_ms;
		record->duty_permille[port] = p->duty_permille;
		record->output_errors[port] = p->output_error;
		record->completed[port] = p->completed;
		record->output_generation[port] = p->output_generation;
	}
}
