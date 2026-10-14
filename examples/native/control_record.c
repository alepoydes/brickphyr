// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU | BP_CAP_BATTERY | BP_CAP_PORTS, 2048);

/* brickphyr.control-input/1: 176 bytes, little endian, signed vectors retain
 * the SDK's units. No pointers or implicit padding. This is a read-only
 * observation example, not a controller or an assumed universal sample rate. */
struct record {
	int64_t capture_us[2], imu_us[2], port_ms[2], angle_mdeg[2];
	int32_t acceleration[3], angular_velocity[3], errors[4];
	uint32_t imu_epoch, imu_sequence, generation[2], battery_mv, resolution_us;
	int64_t applied_ms[2];
	int32_t duty_permille[2], output_errors[2];
	uint32_t completed[2], output_generation[2];
};
_Static_assert(sizeof(struct record) == 176, "Control record layout");
static struct bp_control_frame frame;

int app_main(void)
{
	const struct bp_record_spec spec = {
		.size = sizeof(struct record), .capacity = 32, .schema = "brickphyr.control-input/1",
	};
	int err = bp_record_begin(&spec);
	if (err) return err;
	int64_t due = bp_time_ms();
	for (unsigned int i = 0; i < 40; i++) {
		err = bp_control_frame_get(3, &frame);
		if (err) return err;
		struct record record = {
			.capture_us = {frame.capture_start_us, frame.capture_end_us},
			.imu_us = {frame.imu.raw.sample_us, frame.imu.raw.read_us},
			.errors = {frame.imu.error, frame.ports[0].error, frame.ports[1].error, frame.battery.error},
			.imu_epoch = frame.imu.epoch, .imu_sequence = frame.imu.raw.sequence,
			.battery_mv = frame.battery.voltage_mv, .resolution_us = frame.capture_resolution_us,
		};
		for (unsigned int axis = 0; axis < 3; axis++) {
			record.acceleration[axis] = frame.imu.acceleration[axis];
			record.angular_velocity[axis] = frame.imu.angular_velocity[axis];
		}
		for (unsigned int port = 0; port < 2; port++) {
			const struct bp_control_port *p = &frame.ports[port];
			record.port_ms[port] = p->sample_ms;
			record.angle_mdeg[port] = p->angle_mdeg;
			record.generation[port] = p->generation;
			record.applied_ms[port] = p->applied_ms;
			record.duty_permille[port] = p->duty_permille;
			record.output_errors[port] = p->output_error;
			record.completed[port] = p->completed;
			record.output_generation[port] = p->output_generation;
		}
		err = bp_record_write(&record, sizeof(record));
		if (err) return err;
		if (i == 39) break;
		due += 20;
		int64_t now = bp_time_ms();
		if (due <= now) due += ((now - due) / 20 + 1) * 20;
		err = bp_sleep(due - now);
		if (err) return err;
	}
	return 0;
}
