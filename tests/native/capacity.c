// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>

BRICKPHYR_APPLICATION();

/* User-owned memory must be zeroed before first use and remain writable across
 * the entire mapped region, including its final page. */
static volatile uint8_t capacity[64U * 1024U];

static uint8_t pattern(size_t index)
{
	return (uint8_t)(index * 37U + (index >> 8) * 13U + 0x5aU);
}

int app_main(void)
{
	for (size_t i = 0; i < sizeof(capacity); i++) {
		if (capacity[i] != 0) return 1;
	}

	for (size_t i = 0; i < sizeof(capacity); i++) {
		capacity[i] = pattern(i);
	}
	for (size_t i = 0; i < sizeof(capacity); i++) {
		if (capacity[i] != pattern(i)) return 2;
	}

	int64_t before = bp_time_ms();
	if (bp_sleep(2)) return 3;
	if (bp_time_ms() < before) return 4;

	static const char success[] = "CAPACITY_OK\n";
	if (bp_write(success, sizeof(success) - 1) != (int)(sizeof(success) - 1)) return 5;
	return 0;
}
