// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>

#ifndef CAPACITY_STACK_SIZE
#define CAPACITY_STACK_SIZE 2048
#endif
#ifndef CAPACITY_STRIDE
#define CAPACITY_STRIDE 1
#endif
BRICKPHYR_APPLICATION_REQUIRES(0, CAPACITY_STACK_SIZE);

/* User-owned memory must be zeroed before first use and remain writable across
 * the entire mapped region, including its final page. */
static volatile uint8_t capacity[64U * 1024U];

static uint8_t pattern(size_t index)
{
	return (uint8_t)(index * 37U + (index >> 8) * 13U + 0x5aU);
}

int app_main(void)
{
	if (capacity[sizeof(capacity) - 1] != 0) return 1;
	for (size_t i = 0; i < sizeof(capacity); i += CAPACITY_STRIDE) {
		if (capacity[i] != 0) return 1;
	}

	for (size_t i = 0; i < sizeof(capacity); i += CAPACITY_STRIDE) {
		capacity[i] = pattern(i);
	}
	capacity[sizeof(capacity) - 1] = pattern(sizeof(capacity) - 1);
	for (size_t i = 0; i < sizeof(capacity); i += CAPACITY_STRIDE) {
		if (capacity[i] != pattern(i)) return 2;
	}
	if (capacity[sizeof(capacity) - 1] != pattern(sizeof(capacity) - 1)) return 2;

	int64_t before = bp_time_ms();
	if (bp_sleep(2)) return 3;
	if (bp_time_ms() < before) return 4;

	static const char success[] = "CAPACITY_OK\n";
	if (bp_write(success, sizeof(success) - 1) != (int)(sizeof(success) - 1)) return 5;
	return 0;
}
