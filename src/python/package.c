// SPDX-License-Identifier: Apache-2.0
#include "python_runtime.h"
#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zcbor_encode.h>

/* Pybricks' reference client emits uint32-LE length, module name + NUL, MPY.
 * Normalize this transport packaging once; imports and stored execution retain
 * the existing file-map implementation. No second module loader is involved. */
struct module {
	const uint8_t *name, *data;
	size_t name_size, size;
};

static bool next(const uint8_t **cursor, const uint8_t *end, struct module *module)
{
	if (end - *cursor < 5) {
		return false;
	}
	module->size = sys_get_le32(*cursor);
	module->name = *cursor + 4;
	const uint8_t *zero = memchr(module->name, 0, end - module->name);
	if (!zero || zero == module->name || zero - module->name > PYTHON_PATH_LIMIT - 4 ||
	    module->size > end - zero - 1) {
		return false;
	}
	module->name_size = zero - module->name;
	module->data = zero + 1;
	*cursor = module->data + module->size;
	return true;
}

int python_pybricks_package(void *data, size_t length, size_t capacity, size_t *size)
{
	*size = 0;
	if (!data || !length || length > capacity || length > PYTHON_PYBRICKS_LIMIT) {
		return -ENOEXEC;
	}
	const uint8_t *start = data, *end = start + length, *cursor = start;
	struct module module;
	size_t count = 0;
	while (cursor < end) {
		if (++count > PYTHON_FILE_LIMIT || !next(&cursor, end, &module) ||
		    python_image_check(module.data, module.size, APPLICATION_MPY)) {
			return -ENOEXEC;
		}
	}
	/* Determine package parents before encoding can overwrite their names. */
	bool packages[PYTHON_FILE_LIMIT] = {0};
	cursor = start;
	for (size_t i = 0; i < count; i++) {
		next(&cursor, end, &module);
		/* A module with children supplies that package's __init__.py. Missing
		 * parents are ordinary namespace packages in MicroPython's importer. */
		const uint8_t *scan = start;
		struct module other;
		while (scan < end && next(&scan, end, &other)) {
			if (other.name_size > module.name_size && other.name[module.name_size] == '.' &&
			    !memcmp(other.name, module.name, module.name_size)) {
				packages[i] = true;
				break;
			}
		}
		if (packages[i] && module.name_size + sizeof("/__init__.mpy") - 1 > PYTHON_PATH_LIMIT) {
			return -ENOEXEC;
		}
	}
	/* Every record grows by at most 13 bytes, plus the entry path/map headers.
	 * Shift input above that total bound so encoding any prefix cannot overwrite
	 * unread records. zcbor's string encoder supports overlapping payloads. The
	 * two bounded moves run outside the allocator/interrupt lock. */
	size_t padding = 13 * count + PYTHON_PATH_LIMIT + 8;
	if (capacity < length || padding > capacity - length) {
		return -EFBIG;
	}
	start = (uint8_t *)data + padding;
	end = start + length;
	memmove((uint8_t *)data + padding, data, length);
	ZCBOR_STATE_E(state, 2, data, length + padding, 1);
	bool ok = zcbor_map_start_encode(state, count + 1);
	cursor = start;
	for (size_t i = 0; ok && i < count; i++) {
		next(&cursor, end, &module);
		char path[PYTHON_PATH_LIMIT + sizeof("/__init__.mpy")];
		for (size_t i = 0; i < module.name_size; i++) {
			path[i] = module.name[i] == '.' ? '/' : module.name[i];
		}
		strcpy(path + module.name_size, packages[i] ? "/__init__.mpy" : ".mpy");
		/* Pybricks executes the first record, regardless of its module name.
		 * Preserve that name so ordinary imports keep their own identity. */
		if (module.name == start + sizeof(uint32_t)) {
			ok = zcbor_tstr_put_lit(state, "") &&
				zcbor_bstr_encode_ptr(state, path, strlen(path));
		}
		ok = ok && zcbor_tstr_encode_ptr(state, path, strlen(path)) &&
			zcbor_bstr_encode_ptr(state, module.data, module.size);
	}
	ok = ok && zcbor_map_end_encode(state, count + 1);
	if (!ok) {
		return -EFBIG;
	}
	*size = state->payload - (uint8_t *)data;
	return 0;
}
