// SPDX-License-Identifier: Apache-2.0
#include <arm_math.h>
#include <brickphyr.h>
#include <stdio.h>
#include <string.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	/* This checks the SDK archive/ABI/loader together, not a second DSP
	 * implementation: a four-tap moving average has this impulse response. */
	static const float32_t coefficients[] = {0.25f, 0.25f, 0.25f, 0.25f};
	float32_t state[8], output[5], input[] = {1, 0, 0, 0, 0};
	arm_fir_instance_f32 filter;
	arm_fir_init_f32(&filter, 4, coefficients, state, 5);
	arm_fir_f32(&filter, input, output, 5);
	for (size_t i = 0; i < 5; i++) {
		if (output[i] != (i < 4 ? 0.25f : 0.0f)) {
			return 1;
		}
	}
	/* snprintf's static libc objects also carry unused ARM unwind metadata. */
	char line[32];
	int length = snprintf(line, sizeof(line), "dsp %ld %ld\n",
		(long)(output[0] * 1000), (long)(output[4] * 1000));
	return length == 10 && !strcmp(line, "dsp 250 0\n") ? 0 : 2;
}
