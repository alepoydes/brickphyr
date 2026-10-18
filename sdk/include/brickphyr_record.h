// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>

#define BP_CAPTURE_CONFIG_SIZE 31
#define BP_CAPTURE_APP_MAX_FIELDS 8
#define BP_CAPTURE_APP_MAX_NAME 23
#define BP_CAPTURE_APP_MAX_CONFIG_SIZE \
	(BP_CAPTURE_CONFIG_SIZE + 1 + BP_CAPTURE_APP_MAX_FIELDS * (2 + BP_CAPTURE_APP_MAX_NAME))

/* brickphyr.capture+app/1 appends selected application values to a capture/2
 * row. Configuration starts with the ordinary 31-byte capture selection, then
 * a field count and (type, name length, ASCII name) for each field. Names carry
 * their units where needed. Types are little-endian struct codes b/B/h/H/i/I/f;
 * f is IEEE binary32. The row appends only the field values, in this order.
 * The application copies values actually used by its control step; the recorder
 * neither polls the controller nor allocates during a write. */
struct bp_capture_app_field {
	const char *name;
	char type;
};

static inline int bp_capture_app_config(const uint8_t base[BP_CAPTURE_CONFIG_SIZE],
	const struct bp_capture_app_field *fields, size_t count, void *data,
	size_t capacity, size_t *value_bytes)
{
	if (!base || !fields || !data || !value_bytes || !count ||
	    count > BP_CAPTURE_APP_MAX_FIELDS) return -EINVAL;
	size_t length = BP_CAPTURE_CONFIG_SIZE + 1, values = 0;
	for (size_t i = 0; i < count; i++) {
		const char *name = fields[i].name;
		if (!name) return -EINVAL;
		size_t name_length = 0;
		while (name_length <= BP_CAPTURE_APP_MAX_NAME && name[name_length]) name_length++;
		if (!name_length || name_length > BP_CAPTURE_APP_MAX_NAME ||
		    !((name[0] >= 'A' && name[0] <= 'Z') ||
		      (name[0] >= 'a' && name[0] <= 'z'))) return -EINVAL;
		for (size_t letter = 1; letter < name_length; letter++) {
			char ch = name[letter];
			if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
			      (ch >= '0' && ch <= '9') || ch == '_')) return -EINVAL;
		}
		for (size_t prior = 0; prior < i; prior++) {
			if (!strcmp(name, fields[prior].name)) return -EINVAL;
		}
		size_t width = fields[i].type == 'b' || fields[i].type == 'B' ? 1 :
			fields[i].type == 'h' || fields[i].type == 'H' ? 2 :
			fields[i].type == 'i' || fields[i].type == 'I' || fields[i].type == 'f' ? 4 : 0;
		if (!width) return -EINVAL;
		values += width;
		length += 2 + name_length;
	}
	if (length > capacity) return -ENOSPC;
	uint8_t *out = data;
	memcpy(out, base, BP_CAPTURE_CONFIG_SIZE);
	out[BP_CAPTURE_CONFIG_SIZE] = count;
	size_t offset = BP_CAPTURE_CONFIG_SIZE + 1;
	for (size_t i = 0; i < count; i++) {
		size_t name_length = strlen(fields[i].name);
		out[offset++] = fields[i].type;
		out[offset++] = name_length;
		memcpy(out + offset, fields[i].name, name_length);
		offset += name_length;
	}
	*value_bytes = values;
	return length;
}

/* Fixed-size application records. schema identifies the application's binary
 * format, including version and byte order; the firmware does not reinterpret it.
 * At most 1024 bytes per record and 32768 bytes total, allocated only on begin.
 * Configuration and optional control metadata share that total budget.
 */
struct bp_record_spec {
	uint32_t size, capacity;
	char schema[48]; /* NUL-terminated printable ASCII, nonempty. */
};

struct bp_record_info {
	struct bp_record_spec spec;
	uint32_t generation;
	uint64_t first, next;
	uint64_t trigger;
	uint32_t config_size, after, parameter_size;
	uint32_t reason;
	bool triggered;
	bool active;
	bool control;
};

/* Independent cursor; reading does not consume records for other readers. */
struct bp_record_request {
	uint64_t index;
	uint32_t generation;
	void *data, *parameters;
	size_t capacity, parameters_capacity;
};

enum bp_record_reason {
	BP_RECORD_OPEN, BP_RECORD_APPLICATION_END, BP_RECORD_TRIGGER_END,
	BP_RECORD_FULL, BP_RECORD_DURATION, BP_RECORD_CANCELLED, BP_RECORD_ERROR,
};
