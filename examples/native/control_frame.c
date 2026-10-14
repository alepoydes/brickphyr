// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU | BP_CAP_BATTERY | BP_CAP_PORTS, 2048);

static struct bp_control_frame frame;

static char *number(char *end, int64_t value)
{
	char digits[20];
	size_t count = 0;
	uint64_t magnitude = value < 0 ? -(uint64_t)value : (uint64_t)value;
	*end++ = ' ';
	if (value < 0) *end++ = '-';
	do {
		digits[count++] = '0' + magnitude % 10;
		magnitude /= 10;
	} while (magnitude);
	while (count) *end++ = digits[--count];
	return end;
}

static int write_line(char *line, char *end)
{
	*end++ = '\n';
	size_t length = end - line;
	/* Acceptance into the output queue is not host delivery. */
	int written = bp_write(line, length);
	return written < 0 ? written : (size_t)written == length ? 0 : -ENOBUFS;
}

int app_main(void)
{
	/* Two observations for illustration, without claiming a fixed sample period. */
	for (unsigned int sample = 0; sample < 2; sample++) {
		int err = bp_control_frame_get((1U << 0) | (1U << 1), &frame);
		if (err) return err;

		char line[128];
		char *end = line;
		*end++ = 'F'; /* capture start/end and resolution, microseconds */
		end = number(end, frame.capture_start_us);
		end = number(end, frame.capture_end_us);
		end = number(end, frame.capture_resolution_us);
		err = write_line(line, end);
		if (err) return err;

		end = line;
		*end++ = 'I'; /* sample/read us, state/raw errors, sequence, epoch */
		end = number(end, frame.imu.raw.sample_us);
		end = number(end, frame.imu.raw.read_us);
		end = number(end, frame.imu.error);
		end = number(end, frame.imu.raw.error);
		end = number(end, frame.imu.raw.sequence);
		end = number(end, frame.imu.epoch);
		err = write_line(line, end);
		if (err) return err;

		end = line;
		*end++ = 'V'; /* sample/read ms, error, sequence, millivolts */
		end = number(end, frame.battery.sample_ms);
		end = number(end, frame.battery.read_ms);
		end = number(end, frame.battery.error);
		end = number(end, frame.battery.sequence);
		end = number(end, frame.battery.voltage_mv);
		err = write_line(line, end);
		if (err) return err;

		for (unsigned int port = 0; port < 2; port++) {
			const struct bp_control_port *value = &frame.ports[port];
			end = line;
			*end++ = 'A' + port; /* sample ms, error, sequence, generation, angle mdeg, valid */
			end = number(end, value->sample_ms);
			end = number(end, value->error);
			end = number(end, value->sequence);
			end = number(end, value->generation);
			end = number(end, value->angle_mdeg);
			end = number(end, value->valid);
			err = write_line(line, end);
			if (err) return err;

			end = line;
			*end++ = 'a' + port; /* requested/applied ms, error, duty, request/completed,
			                    * pending, request/output generations */
			end = number(end, value->requested_ms);
			end = number(end, value->applied_ms);
			end = number(end, value->output_error);
			end = number(end, value->duty_permille);
			end = number(end, value->request);
			end = number(end, value->completed);
			end = number(end, value->pending);
			end = number(end, value->request_generation);
			end = number(end, value->output_generation);
			err = write_line(line, end);
			if (err) return err;
		}
		if (sample == 0) {
			err = bp_sleep(100);
			if (err) return err;
		}
	}
	return 0;
}
