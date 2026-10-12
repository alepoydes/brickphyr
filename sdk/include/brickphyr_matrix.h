// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#define BP_MATRIX_PIXELS 25
#define BP_MATRIX_MAX_FRAMES 255

enum bp_matrix_owner { BP_MATRIX_FREE, BP_MATRIX_APPLICATION, BP_MATRIX_TERMINAL };

/* Brightness is 0..100, row-major, top left first in the upright hub orientation.
 * Applied values mean a successful controller write, not measured light output.
 * After an I/O failure valid is false until the next successful write.
 */
struct bp_matrix_state {
	int64_t applied_ms;
	int32_t error;
	uint32_t errors;
	uint8_t requested[BP_MATRIX_PIXELS];
	uint8_t applied[BP_MATRIX_PIXELS];
	uint8_t owner;
	uint8_t pending;
	uint8_t valid;
	uint8_t animating;
};
