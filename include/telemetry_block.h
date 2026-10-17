// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

/* One wire field in a fully packed capture row. Layout is fixed at admission. */
struct telemetry_wire_field {
	uint16_t offset;
	uint8_t size;
	int16_t alias;
	uint8_t delta;
};

/* Caller owns all storage. A block starts with a complete row; following rows
 * contain a field-change bitmap and exact replacement bytes. No allocation or
 * transport operation takes place in append(). */
struct telemetry_block {
	uint8_t *data, *previous;
	const struct telemetry_wire_field *fields;
	size_t capacity, length, row_size, field_count;
	uint16_t rows;
};

int telemetry_block_init(struct telemetry_block *block, uint8_t *data, size_t capacity,
	uint8_t *previous, size_t row_size, const struct telemetry_wire_field *fields,
	size_t field_count);
/* ENOSPC leaves the block unchanged: publish it, then initialize the next one
 * and append this row. EMSGSIZE means a complete first row cannot fit. */
int telemetry_block_append(struct telemetry_block *block, const uint8_t *row);
