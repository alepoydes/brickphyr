// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	/* Keep distinct values live in every callee-saved FP register across
	 * userspace API calls, blocking and preemption by the fixture's peer. */
	for (unsigned int pass = 0; pass < 4; pass++) {
		uint32_t expected[16], observed[16];
		for (unsigned int i = 0; i < 16; i++) {
			expected[i] = 0x3f800000U + (pass << 16) + (i << 12);
		}
		__asm__ volatile("vldmia %0, {s16-s31}" : : "r"(expected) :
			"s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
			"s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31", "memory");
		if (bp_sleep(2)) {
			return 1;
		}
		(void)bp_time_ms();
		__asm__ volatile("vstmia %0, {s16-s31}" : : "r"(observed) : "memory");
		for (unsigned int i = 0; i < 16; i++) {
			if (observed[i] != expected[i]) {
				return 2;
			}
		}
	}
	return 0;
}
