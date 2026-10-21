// SPDX-License-Identifier: Apache-2.0
#include "app_package.h"
#include "application.h"
#include <errno.h>
#include <string.h>
#include <zephyr/sys/util.h>

int app_source_read(const struct app_source *source, size_t offset, void *data, size_t length)
{
	if (offset > source->length || length > source->length - offset) return -ENOEXEC;
	return length ? source->read(source->context, offset, data, length) : 0;
}

int app_source_memory(void *context, size_t offset, void *data, size_t length)
{
	memcpy(data, (const uint8_t *)context + offset, length);
	return 0;
}

#ifdef CONFIG_BRICKPHYR_APP_PACKAGES
#include <zcbor_decode.h>

/* zcbor decodes the existing file map in bounded windows. Resource payloads
 * need no buffering: their validated lengths advance the next window directly. */
struct cursor {
	const struct app_source *source;
	zcbor_state_t state[4];
	uint8_t buffer[APP_PATH_LIMIT + 9];
	size_t offset, count, limit;
	char name[APP_PATH_LIMIT + 1];
	struct app_resource file;
};

static int window(struct cursor *cursor, size_t offset)
{
	if (offset > cursor->limit) return -ENOEXEC;
	size_t size = MIN(sizeof(cursor->buffer), cursor->limit - offset);
	int err = app_source_read(cursor->source, offset, cursor->buffer, size);
	if (err) return err;
	cursor->offset = offset;
	zcbor_update_state(cursor->state, cursor->buffer, size);
	return 0;
}

static size_t position(const struct cursor *cursor)
{
	return cursor->offset + (cursor->state[0].payload - cursor->buffer);
}

static int begin(struct cursor *cursor, const struct app_source *source)
{
	*cursor = (struct cursor){.source = source, .limit = source->length};
	zcbor_new_decode_state(cursor->state, ARRAY_SIZE(cursor->state), cursor->buffer, 0, 1, NULL, 0);
	int err = window(cursor, 0);
	return err ? err : zcbor_map_start_decode(cursor->state) ? 0 : -ENOEXEC;
}

static bool path_valid(const char *path, size_t length)
{
	if (!length || length > APP_PATH_LIMIT || memchr(path, 0, length)) return false;
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
	if (++cursor->count > APP_FILE_LIMIT + 1) return -E2BIG;
	struct zcbor_string name;
	if (!zcbor_tstr_decode(cursor->state, &name) ||
	    (name.len && !path_valid((const char *)name.value, name.len))) return -ENOEXEC;
	memcpy(cursor->name, name.value, name.len);
	cursor->name[name.len] = 0;
	err = window(cursor, position(cursor));
	if (err) return err;
	struct zcbor_string_fragment data;
	if (!zcbor_bstr_decode_fragment(cursor->state, &data)) return -ENOEXEC;
	cursor->file = (struct app_resource){
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
	int err = app_source_read(cursor->source, cursor->file.offset, path, cursor->file.length);
	if (err) return err;
	if (!path_valid(path, cursor->file.length)) return -ENOEXEC;
	path[cursor->file.length] = 0;
	return 0;
}

int app_package_find(const struct app_source *source, const char *path, struct app_resource *file)
{
	if (*path == '/') path++;
	if (!*path) return APP_RESOURCE_DIR;
	if (!path_valid(path, strlen(path))) return APP_RESOURCE_MISSING;
	struct cursor cursor;
	int err = begin(&cursor, source);
	if (err) return err;
	while ((err = next(&cursor)) > 0) {
		if (!cursor.name[0]) continue;
		if (!strcmp(path, cursor.name)) {
			*file = cursor.file;
			return APP_RESOURCE_FILE;
		}
		if (prefix(path, cursor.name)) return APP_RESOURCE_DIR;
	}
	return err;
}

static enum application_format file_format(const char *path)
{
	size_t length = strlen(path);
	if (length > 3 && !strcmp(path + length - 3, ".py")) return APPLICATION_PYTHON;
	if (length > 4 && !strcmp(path + length - 4, ".mpy")) return APPLICATION_MPY;
	if (length > 4 && !strcmp(path + length - 4, ".elf")) return APPLICATION_NATIVE;
	return APPLICATION_FILES;
}

static int text_path(struct cursor *cursor, char *path)
{
	int err = window(cursor, position(cursor));
	if (err) return err;
	struct zcbor_string value;
	if (!zcbor_tstr_decode(cursor->state, &value) ||
	    !path_valid((const char *)value.value, value.len)) return -ENOEXEC;
	memcpy(path, value.value, value.len);
	path[value.len] = 0;
	return 0;
}

/* The empty map key retains legacy entry-name bytes, or contains the versioned
 * CBOR manifest [1, engine, entry, [required native component paths]]. Neither
 * the manifest nor component payloads need a whole-file allocation. */
static int metadata(struct cursor *cursor, struct app_package_info *info)
{
	size_t offset = cursor->file.offset;
	if (!cursor->file.length) return -ENOEXEC;
	cursor->limit = offset + cursor->file.length;
	zcbor_new_decode_state(cursor->state, ARRAY_SIZE(cursor->state), cursor->buffer, 0, 1, NULL, 0);
	int err = window(cursor, offset);
	if (err) return err;
	if (cursor->buffer[0] != 0x84) {
		err = entry_name(cursor, info->entry, sizeof(info->entry));
		if (err) return err;
		info->format = file_format(info->entry);
		return info->format == APPLICATION_PYTHON || info->format == APPLICATION_MPY ? 0 : -ENOEXEC;
	}
	uint32_t version, format;
	if (!zcbor_list_start_decode(cursor->state) || !zcbor_uint32_decode(cursor->state, &version)) return -ENOEXEC;
	if (version != 1) return -ENOTSUP;
	if (!zcbor_uint32_decode(cursor->state, &format) || format > APPLICATION_MPY) return -ENOEXEC;
	info->format = format;
	err = text_path(cursor, info->entry);
	if (err) return err;
	if (file_format(info->entry) != info->format) return -ENOEXEC;
	err = window(cursor, position(cursor));
	if (err || !zcbor_list_start_decode(cursor->state)) return err ? err : -ENOEXEC;
	for (size_t count = 0; !zcbor_array_at_end(cursor->state); count++) {
		if (count >= APP_FILE_LIMIT) return -E2BIG;
		char path[APP_PATH_LIMIT + 1];
		err = text_path(cursor, path);
		if (err) return err;
		struct app_resource resource;
		int found = app_package_find(cursor->source, path, &resource);
		if (found != APP_RESOURCE_FILE) return found < 0 ? found : -ENOENT;
		if (file_format(path) != APPLICATION_NATIVE || !strcmp(path, info->entry)) return -ENOEXEC;
		err = window(cursor, position(cursor));
		if (err) return err;
	}
	return zcbor_list_end_decode(cursor->state) && zcbor_list_end_decode(cursor->state) &&
		position(cursor) == cursor->limit ? 0 : -ENOEXEC;
}

static int package_info(const struct app_source *source, struct app_package_info *info)
{
	struct cursor cursor;
	int err = begin(&cursor, source);
	if (err) return err;
	const char *fallback = NULL;
	while ((err = next(&cursor)) > 0) {
		if (!cursor.name[0]) return metadata(&cursor, info);
		if (!strcmp(cursor.name, "main.py")) fallback = "main.py";
		if (!strcmp(cursor.name, "main.mpy")) fallback = "main.mpy";
	}
	if (err) return err;
	if (!fallback) return -ENOEXEC;
	strcpy(info->entry, fallback);
	info->format = file_format(fallback);
	return 0;
}

int app_package_entry(const struct app_source *source, char *path, size_t capacity)
{
	struct app_package_info info;
	int err = package_info(source, &info);
	if (err) return err;
	if (strlen(info.entry) >= capacity) return -ENAMETOOLONG;
	strcpy(path, info.entry);
	return 0;
}

/* Admission and manifest resolution use separate stack frames: their cursors
 * never need to coexist through filesystem and SPI driver calls. */
static __noinline int check_files(const struct app_source *source, app_package_check_fn check,
	const struct app_package_info *info)
{
	struct cursor cursor;
	int err = begin(&cursor, source);
	if (err) return err;
	size_t metadata_count = 0, entries = 0, defaults = 0;
	while ((err = next(&cursor)) > 0) {
		if (!cursor.name[0]) {
			if (metadata_count++) return -ENOEXEC;
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
		bool entry = !strcmp(info->entry, cursor.name);
		enum application_format format = entry ? info->format : file_format(cursor.name);
		if (!entry && info->format == APPLICATION_NATIVE && format != APPLICATION_NATIVE) format = APPLICATION_FILES;
		if (check) {
			err = check(source, cursor.file, format);
			if (err) return err;
		}
		entries += entry;
		defaults += !strcmp(cursor.name, "main.py") || !strcmp(cursor.name, "main.mpy");
	}
	if (err) return err;
	if (cursor.count - metadata_count > APP_FILE_LIMIT) return -E2BIG;
	return entries == 1 && (metadata_count || defaults == 1) ? 0 : -ENOEXEC;
}

int app_package_check(const struct app_source *source, app_package_check_fn check,
	struct app_package_info *info)
{
	int err = package_info(source, info);
	return err ? err : check_files(source, check, info);
}
#else
int app_package_find(const struct app_source *source, const char *path, struct app_resource *file)
{
	return -ENOTSUP;
}

int app_package_entry(const struct app_source *source, char *path, size_t capacity)
{
	return -ENOTSUP;
}

int app_package_check(const struct app_source *source, app_package_check_fn check,
	struct app_package_info *info)
{
	return -ENOTSUP;
}
#endif
