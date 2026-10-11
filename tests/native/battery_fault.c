// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	struct bp_battery_sample value = {0};
	int64_t deadline = bp_time_ms() + 1500;
	for (;;) {
		int err = bp_battery_get(&value);
		if (err) {
			return err;
		}
		if (value.error != -EINPROGRESS) {
			break;
		}
		if (bp_time_ms() >= deadline) {
			return -ETIMEDOUT;
		}
		bp_sleep(10);
	}
	if (value.error || !value.sequence || value.sample_ms < 1000 ||
	    value.read_ms < value.sample_ms || value.current_raw >= 4096 || value.voltage_raw >= 4096) {
		return -ERANGE;
	}
	/* This whole range is writable user memory, but not aligned for a struct.
	 * Reject it before entering the service's snapshot lock. */
	uint64_t storage[sizeof(value) / sizeof(uint64_t) + 1];
	return bp_battery_get((struct bp_battery_sample *)((uint8_t *)storage + 1));
}
