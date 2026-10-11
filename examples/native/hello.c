// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>

BRICKPHYR_APPLICATION();

static volatile unsigned int seed = 7;
static volatile unsigned int initialized;
static volatile float gain = 1.5f;

static void __attribute__((constructor)) initialize(void)
{
	initialized = seed;
}

int app_main(void)
{
	int64_t start = bp_time_ms();
	/* Ordinary C initialization can emit a call to the static C library. */
	char line[96] = "Hello from a native application.\n";
	bp_write(line, 33);
	bp_sleep(20);
	return initialized == 7 && gain * gain == 2.25f && line[95] == 0 &&
		bp_time_ms() >= start + 20 ? 0 : 1;
}
