// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One wire field in a fully packed capture row. Layout is fixed at admission. */
struct telemetry_wire_field {
	uint16_t offset;
	uint8_t size;
	int16_t alias;
	uint8_t delta;
};

/* Caller owns all storage. The first row is independently decodable: BCB3
 * stores full fields, while opt-in BCB4 stores a nibble width per field and
 * low-order bytes, with zero-filled high bytes. BCB4 is chosen only if smaller.
 * Following rows use the same exact field changes in both formats. append()
 * neither allocates nor performs transport operations. */
struct telemetry_block {
	uint8_t *data, *previous;
	const struct telemetry_wire_field *fields;
	size_t capacity, length, row_size, field_count;
	uint16_t rows;
	bool compact_first;
};

int telemetry_block_init(struct telemetry_block *block, uint8_t *data, size_t capacity,
	uint8_t *previous, size_t row_size, const struct telemetry_wire_field *fields,
	size_t field_count, bool compact_first);
/* ENOSPC leaves the block unchanged: publish it, then initialize the next one
 * and append this row. EMSGSIZE means a complete first row cannot fit. */
int telemetry_block_append(struct telemetry_block *block, const uint8_t *row);
