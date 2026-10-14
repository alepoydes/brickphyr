// SPDX-License-Identifier: Apache-2.0
#include <dsp/filtering_functions.h>
#include <string.h>

/* One recorded FIR step. The caller owns coefficient/state buffers and outputs. */
void brickphyr_imu_fir_step(const float32_t coefficients[4],
			   const float32_t before[4], float32_t input,
			   float32_t *output, float32_t after[4])
{
	arm_fir_instance_f32 filter = {
		.numTaps = 4,
		.pState = after,
		.pCoeffs = coefficients,
	};

	memmove(after, before, 4 * sizeof(float32_t));
	arm_fir_f32(&filter, &input, output, 1);
}
