// SPDX-License-Identifier: Apache-2.0
#include "python_runtime.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
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

int python_pybricks_package(const void *data, size_t length, void **image, size_t *size)
{
	*image = NULL;
	*size = 0;
	if (!data || !length || length > PYTHON_PYBRICKS_LIMIT) {
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
	/* Each file gains at most 13 bytes (package suffix and CBOR headers).
	 * Reserve the explicit entry path and map headers as well. */
	size_t capacity = MIN(APPLICATION_IMAGE_LIMIT,
		length + 13 * count + PYTHON_PATH_LIMIT + 8);
	void *buffer = k_malloc(capacity);
	if (!buffer) {
		return -ENOMEM;
	}
	ZCBOR_STATE_E(state, 2, buffer, capacity, 1);
	bool ok = zcbor_map_start_encode(state, count + 1);
	cursor = start;
	while (ok && cursor < end && next(&cursor, end, &module)) {
		/* A module with children supplies that package's __init__.py. Missing
		 * parents are ordinary namespace packages in MicroPython's importer. */
		bool package = false;
		const uint8_t *scan = start;
		struct module other;
		while (scan < end && next(&scan, end, &other)) {
			if (other.name_size > module.name_size && other.name[module.name_size] == '.' &&
			    !memcmp(other.name, module.name, module.name_size)) {
				package = true;
				break;
			}
		}
		char path[PYTHON_PATH_LIMIT + sizeof("/__init__.mpy")];
		for (size_t i = 0; i < module.name_size; i++) {
			path[i] = module.name[i] == '.' ? '/' : module.name[i];
		}
		strcpy(path + module.name_size, package ? "/__init__.mpy" : ".mpy");
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
	size_t used = state->payload - (uint8_t *)buffer;
	if (!ok) {
		k_free(buffer);
		return -EFBIG;
	}
	*image = buffer;
	*size = used;
	return 0;
}
