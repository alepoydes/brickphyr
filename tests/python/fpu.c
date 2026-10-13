// SPDX-License-Identifier: Apache-2.0
#include "py/nlr.h"
#include <zephyr/ztest.h>

ZTEST_USER(python, test_fp_exception_context)
{
	uint32_t expected[16], changed[16], observed[16];
	for (unsigned int i = 0; i < ARRAY_SIZE(expected); i++) {
		expected[i] = 0x3f800000U + (i << 12);
		changed[i] = 0x42000000U + (i << 12);
	}
	__asm__ volatile("vldmia %0, {s16-s31}" : : "r"(expected) :
		"s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
		"s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31", "memory");
	nlr_buf_t frame;
	if (!nlr_push(&frame)) {
		/* An exception bypasses the callee's normal FP-register restoration. */
		__asm__ volatile("vldmia %0, {s16-s31}" : : "r"(changed) :
			"s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
			"s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31", "memory");
		nlr_jump(&frame);
	}
	__asm__ volatile("vstmia %0, {s16-s31}" : : "r"(observed) : "memory");
	zassert_mem_equal(expected, observed, sizeof(expected));
}
