// SPDX-License-Identifier: Apache-2.0
#include "telemetry_block.h"

#include <errno.h>
#include <string.h>

enum { HEADER_SIZE = 8, MAX_FIELDS = 256 };

static int64_t value(const uint8_t *data, size_t size)
{
	uint64_t raw = 0;
	for (size_t i = 0; i < size; i++) raw |= (uint64_t)data[i] << (8 * i);
	if (size == 8 && raw & (UINT64_C(1) << 63)) return -1 - (int64_t)~raw;
	return (int64_t)raw;
}

static unsigned int delta_bytes(const uint8_t *current, const uint8_t *previous, size_t size,
	int64_t *delta)
{
	if (__builtin_sub_overflow(value(current, size), value(previous, size), delta)) return 0;
	for (unsigned int bytes = 1; bytes < size; bytes++) {
		int64_t limit = INT64_C(1) << (8 * bytes - 1);
		if (*delta >= -limit && *delta < limit) return bytes;
	}
	return 0;
}

int telemetry_block_init(struct telemetry_block *block, uint8_t *data, size_t capacity,
	uint8_t *previous, size_t row_size, const struct telemetry_wire_field *fields,
	size_t field_count)
{
	if (!block || !data || !previous || !fields || !field_count ||
	    field_count > MAX_FIELDS || !row_size || row_size > UINT16_MAX ||
	    capacity > 1024 || capacity < HEADER_SIZE + row_size) return -EINVAL;
	size_t end = 0;
	for (size_t i = 0; i < field_count; i++) {
		if (!fields[i].size || fields[i].offset != end || fields[i].alias >= (int)i ||
		    fields[i].alias < -1 ||
		    (fields[i].delta && fields[i].size != 4 && fields[i].size != 8) ||
		    (fields[i].alias >= 0 && fields[fields[i].alias].size != fields[i].size)) return -EINVAL;
		end += fields[i].size;
	}
	if (end != row_size) return -EINVAL;
	*block = (struct telemetry_block){.data = data, .previous = previous,
		.fields = fields, .capacity = capacity, .length = HEADER_SIZE,
		.row_size = row_size, .field_count = field_count};
	memcpy(data, "BCB3", 4);
	data[4] = data[5] = 0;
	data[6] = row_size;
	data[7] = row_size >> 8;
	return 0;
}

int telemetry_block_append(struct telemetry_block *block, const uint8_t *row)
{
	if (!block || !row || !block->data || !block->previous || !block->fields) return -EINVAL;
	if (block->rows == 255) return -ENOSPC;
	for (size_t i = 0; i < block->field_count; i++) {
		const struct telemetry_wire_field *field = &block->fields[i];
		if (field->alias >= 0 && memcmp(row + field->offset,
			row + block->fields[field->alias].offset, field->size)) return -EINVAL;
	}
	if (!block->rows) {
		for (size_t i = 0; i < block->field_count; i++) {
			const struct telemetry_wire_field *field = &block->fields[i];
			if (field->alias >= 0) continue;
			memcpy(block->data + block->length, row + field->offset, field->size);
			block->length += field->size;
		}
	} else {
		uint8_t mask[MAX_FIELDS / 8] = {0};
		size_t mask_size = (block->field_count + 7) / 8;
		size_t needed = mask_size;
		for (size_t i = 0; i < block->field_count; i++) {
			const struct telemetry_wire_field *field = &block->fields[i];
			if (memcmp(row + field->offset, block->previous + field->offset, field->size)) {
				mask[i / 8] |= 1U << (i % 8);
			if (field->alias < 0) {
				int64_t delta;
				unsigned int bytes = field->delta ?
					delta_bytes(row + field->offset, block->previous + field->offset,
						field->size, &delta) : 0;
				needed += field->size + (field->delta ? 1 : 0) -
					(bytes ? field->size - bytes : 0);
			}
			}
		}
		if (needed > block->capacity - block->length) return -ENOSPC;
		memcpy(block->data + block->length, mask, mask_size);
		block->length += mask_size;
		for (size_t i = 0; i < block->field_count; i++) {
			if (!(mask[i / 8] & (1U << (i % 8)))) continue;
			const struct telemetry_wire_field *field = &block->fields[i];
			if (field->alias >= 0) continue;
			int64_t delta;
			unsigned int bytes = field->delta ?
				delta_bytes(row + field->offset, block->previous + field->offset,
					field->size, &delta) : 0;
			if (field->delta) block->data[block->length++] = bytes;
			uint64_t encoded = bytes ? (uint64_t)delta : 0;
			for (unsigned int part = 0; part < bytes; part++) {
				block->data[block->length++] = encoded >> (8 * part);
			}
			if (!bytes) {
				memcpy(block->data + block->length, row + field->offset, field->size);
				block->length += field->size;
			}
		}
	}
	memcpy(block->previous, row, block->row_size);
	block->rows++;
	block->data[4] = block->rows;
	block->data[5] = block->rows >> 8;
	return 0;
}
