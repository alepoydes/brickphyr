// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <brickphyr.h>

BRICKPHYR_APPLICATION();

static char *number(char *end, int32_t value)
{
	char digits[10];
	size_t count = 0;
	uint32_t magnitude = value < 0 ? -(int64_t)value : value;
	*end++ = ' ';
	if (value < 0) {
		*end++ = '-';
	}
	do {
		digits[count++] = '0' + magnitude % 10;
		magnitude /= 10;
	} while (magnitude);
	while (count) {
		*end++ = digits[--count];
	}
	return end;
}

int app_main(void)
{
	for (;;) {
		struct bp_imu_sample sample;
		int err = bp_imu_get(&sample);
		if (err) {
			return err;
		}
		if (sample.error == -EINPROGRESS || sample.error == -EAGAIN) {
			bp_sleep(10);
			continue;
		}
		if (sample.error) {
			return sample.error;
		}
		char line[96] = "imu um/s2 mdps:";
		char *end = line + sizeof("imu um/s2 mdps:") - 1;
		for (size_t i = 0; i < 3; i++) {
			end = number(end, sample.acceleration[i]);
		}
		for (size_t i = 0; i < 3; i++) {
			end = number(end, sample.angular_velocity[i]);
		}
		*end++ = '\n';
		bp_write(line, end - line);
		bp_sleep(200);
	}
}
