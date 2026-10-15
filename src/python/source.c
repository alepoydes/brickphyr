// SPDX-License-Identifier: Apache-2.0
#include "python_source.h"
#include "python_runtime.h"
#include <errno.h>
#include <string.h>
#include <zephyr/sys/util.h>
#include <zcbor_decode.h>

int python_source_read(const struct python_source *source, size_t offset, void *data, size_t length)
{
	if (offset > source->length || length > source->length - offset) return -ENOEXEC;
	return length ? source->read(source->context, offset, data, length) : 0;
}

int python_source_memory(void *context, size_t offset, void *data, size_t length)
{
	memcpy(data, (const uint8_t *)context + offset, length);
	return 0;
}

/* zcbor decodes the existing file map in bounded windows. Resource payloads
 * need no buffering: their validated lengths advance the next window directly. */
struct cursor {
	const struct python_source *source;
	zcbor_state_t state[3];
	uint8_t buffer[PYTHON_PATH_LIMIT + 9];
	size_t offset, count;
	char name[PYTHON_PATH_LIMIT + 1];
	struct python_resource file;
};

static int window(struct cursor *cursor, size_t offset)
{
	if (offset > cursor->source->length) return -ENOEXEC;
	size_t size = MIN(sizeof(cursor->buffer), cursor->source->length - offset);
	int err = python_source_read(cursor->source, offset, cursor->buffer, size);
	if (err) return err;
	cursor->offset = offset;
	zcbor_update_state(cursor->state, cursor->buffer, size);
	return 0;
}

static size_t position(const struct cursor *cursor)
{
	return cursor->offset + (cursor->state[0].payload - cursor->buffer);
}

static int begin(struct cursor *cursor, const struct python_source *source)
{
	*cursor = (struct cursor){.source = source};
	zcbor_new_decode_state(cursor->state, ARRAY_SIZE(cursor->state), cursor->buffer, 0, 1, NULL, 0);
	int err = window(cursor, 0);
	return err ? err : zcbor_map_start_decode(cursor->state) ? 0 : -ENOEXEC;
}

static bool path_valid(const char *path, size_t length)
{
	if (!length || length > PYTHON_PATH_LIMIT || memchr(path, 0, length)) return false;
	for (size_t start = 0, end = 0; end <= length; end++) {
		if (end != length && path[end] != '/') continue;
		size_t size = end - start;
		if (!size || (path[start] == '.' && (size == 1 || (size == 2 && path[start + 1] == '.')))) return false;
		start = end + 1;
	}
	return true;
}

/* One on success, zero at the exact end of the map, negative on failure. */
static int next(struct cursor *cursor)
{
	int err = window(cursor, position(cursor));
	if (err) return err;
	if (zcbor_array_at_end(cursor->state)) {
		return zcbor_map_end_decode(cursor->state) && position(cursor) == cursor->source->length ? 0 : -ENOEXEC;
	}
	if (++cursor->count > PYTHON_FILE_LIMIT + 1) return -E2BIG;
	struct zcbor_string name;
	if (!zcbor_tstr_decode(cursor->state, &name) ||
	    (name.len && !path_valid((const char *)name.value, name.len))) return -ENOEXEC;
	memcpy(cursor->name, name.value, name.len);
	cursor->name[name.len] = 0;
	err = window(cursor, position(cursor));
	if (err) return err;
	struct zcbor_string_fragment data;
	if (!zcbor_bstr_decode_fragment(cursor->state, &data)) return -ENOEXEC;
	cursor->file = (struct python_resource){
		.offset = cursor->offset + (data.fragment.value - cursor->buffer), .length = data.total_len,
	};
	if (cursor->file.offset > cursor->source->length ||
	    data.total_len > cursor->source->length - cursor->file.offset) return -ENOEXEC;
	err = window(cursor, cursor->file.offset + data.total_len);
	return err ? err : 1;
}

static bool prefix(const char *first, const char *second)
{
	size_t size = strlen(first);
	return size < strlen(second) && second[size] == '/' && !memcmp(first, second, size);
}

static int entry_name(const struct cursor *cursor, char *path, size_t capacity)
{
	if (cursor->file.length >= capacity) return -ENOEXEC;
	int err = python_source_read(cursor->source, cursor->file.offset, path, cursor->file.length);
	if (err) return err;
	if (!path_valid(path, cursor->file.length)) return -ENOEXEC;
	path[cursor->file.length] = 0;
	return 0;
}

int python_source_find(const struct python_source *source, const char *path, struct python_resource *file)
{
	if (*path == '/') path++;
	if (!*path) return PYTHON_RESOURCE_DIR;
	if (!path_valid(path, strlen(path))) return PYTHON_RESOURCE_MISSING;
	struct cursor cursor;
	int err = begin(&cursor, source);
	if (err) return err;
	while ((err = next(&cursor)) > 0) {
		if (!cursor.name[0]) continue;
		if (!strcmp(path, cursor.name)) {
			*file = cursor.file;
			return PYTHON_RESOURCE_FILE;
		}
		if (prefix(path, cursor.name)) return PYTHON_RESOURCE_DIR;
	}
	return err;
}

int python_source_entry(const struct python_source *source, char *path, size_t capacity)
{
	struct cursor cursor;
	int err = begin(&cursor, source);
	if (err) return err;
	const char *fallback = NULL;
	while ((err = next(&cursor)) > 0) {
		if (!cursor.name[0]) return entry_name(&cursor, path, capacity);
		if (!strcmp(cursor.name, "main.py")) fallback = "main.py";
		if (!strcmp(cursor.name, "main.mpy")) fallback = "main.mpy";
	}
	if (err) return err;
	if (!fallback || strlen(fallback) >= capacity) return -ENOEXEC;
	strcpy(path, fallback);
	return 0;
}

static int check_code(const struct python_source *source, struct python_resource file, bool mpy)
{
	uint8_t buffer[128];
	if (mpy) {
		if (file.length < 4) return -ENOEXEC;
		int err = python_source_read(source, file.offset, buffer, 4);
		return err ? err : python_image_check(buffer, 4, APPLICATION_MPY);
	}
	while (file.length) {
		size_t size = MIN(file.length, sizeof(buffer));
		int err = python_source_read(source, file.offset, buffer, size);
		if (err) return err;
		if (memchr(buffer, 0, size)) return -ENOEXEC;
		file.offset += size;
		file.length -= size;
	}
	return 0;
}

int python_source_check(const struct python_source *source)
{
	char entry[PYTHON_PATH_LIMIT + 1];
	int err = python_source_entry(source, entry, sizeof(entry));
	if (err) return err;
	struct cursor cursor;
	err = begin(&cursor, source);
	if (err) return err;
	size_t metadata = 0, entries = 0, defaults = 0;
	while ((err = next(&cursor)) > 0) {
		if (!cursor.name[0]) {
			if (metadata++) return -ENOEXEC;
			continue;
		}
		/* Admission is cold and bounded to 64 files. Keep no permanent index. */
		struct cursor previous;
		err = begin(&previous, source);
		if (err) return err;
		for (size_t i = 1; i < cursor.count; i++) {
			err = next(&previous);
			if (err <= 0) return err ? err : -ENOEXEC;
			if (!strcmp(cursor.name, previous.name) || prefix(cursor.name, previous.name) ||
			    prefix(previous.name, cursor.name)) return -ENOEXEC;
		}
		size_t length = strlen(cursor.name);
		bool mpy = length > 4 && !strcmp(cursor.name + length - 4, ".mpy");
		bool python = length > 3 && !strcmp(cursor.name + length - 3, ".py");
		if (mpy || python) {
			err = check_code(source, cursor.file, mpy);
			if (err) return err;
			entries += !strcmp(entry, cursor.name);
			defaults += !strcmp(cursor.name, "main.py") || !strcmp(cursor.name, "main.mpy");
		}
	}
	if (err) return err;
	if (cursor.count - metadata > PYTHON_FILE_LIMIT) return -E2BIG;
	return entries == 1 && (metadata || defaults == 1) ? 0 : -ENOEXEC;
}
