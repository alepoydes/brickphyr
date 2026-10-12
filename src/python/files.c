// SPDX-License-Identifier: Apache-2.0 AND MIT
#include "files.h"
#include "python_runtime.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <zephyr/sys/util.h>
#include <zcbor_decode.h>
#include "py/builtin.h"
#include "py/lexer.h"
#include "py/runtime.h"
#include "py/stream.h"

/* The read-only stream below adapts MicroPython extmod/vfs_rom_file.c,
 * Copyright (c) 2022 Damien P. George, MIT. Storage lookup remains this port's
 * existing program namespace; no VFS or second filesystem is instantiated. */

/* Bound admission work without allocating a second file index. The image itself
 * lives with the application's heap and supplies all import bytes. */
static const void *files;
static size_t files_size;

static bool path_valid(const char *path, size_t length)
{
	if (!length || length > PYTHON_PATH_LIMIT || memchr(path, 0, length)) {
		return false;
	}
	for (size_t start = 0, end = 0; end <= length; end++) {
		if (end != length && path[end] != '/') {
			continue;
		}
		size_t n = end - start;
		if (!n || (path[start] == '.' && (n == 1 || (n == 2 && path[start + 1] == '.')))) {
			return false;
		}
		start = end + 1;
	}
	return true;
}

static bool prefix(const struct zcbor_string *a, const struct zcbor_string *b)
{
	return a->len < b->len && b->value[a->len] == '/' && !memcmp(a->value, b->value, a->len);
}

static bool explicit_entry(const void *image, size_t length, struct zcbor_string *entry)
{
	*entry = (struct zcbor_string){0};
	ZCBOR_STATE_D(state, 2, image, length, 1, 0);
	if (!zcbor_map_start_decode(state)) {
		return false;
	}
	for (size_t count = 0; !zcbor_array_at_end(state); count++) {
		struct zcbor_string name, data;
		if (count > PYTHON_FILE_LIMIT || !zcbor_tstr_decode(state, &name) ||
		    !zcbor_bstr_decode(state, &data)) {
			return false;
		}
		if (!name.len) {
			if (entry->len || !path_valid((const char *)data.value, data.len)) {
				return false;
			}
			*entry = data;
		}
	}
	return zcbor_map_end_decode(state) && zcbor_payload_at_end(state);
}

int python_files_check(const void *image, size_t length)
{
	struct zcbor_string entry;
	if (!explicit_entry(image, length, &entry)) {
		return -ENOEXEC;
	}
	ZCBOR_STATE_D(state, 2, image, length, 1, 0);
	if (!zcbor_map_start_decode(state)) {
		return -ENOEXEC;
	}
	size_t count = 0, entries = 0;
	while (!zcbor_array_at_end(state)) {
		struct zcbor_string name, data;
		if (++count > PYTHON_FILE_LIMIT + !!entry.len) {
			return -E2BIG;
		}
		if (!zcbor_tstr_decode(state, &name) || !zcbor_bstr_decode(state, &data)) {
			return -ENOEXEC;
		}
		if (!name.len) {
			continue;
		}
		if (!path_valid((const char *)name.value, name.len)) {
			return -ENOEXEC;
		}
		/* Reject duplicate names and file/directory collisions. Both would
		 * otherwise make the selected code depend on lookup order. */
		ZCBOR_STATE_D(previous, 2, image, length, 1, 0);
		if (!zcbor_map_start_decode(previous)) {
			return -ENOEXEC;
		}
		for (size_t i = 1; i < count; i++) {
			struct zcbor_string earlier, ignored;
			if (!zcbor_tstr_decode(previous, &earlier) || !zcbor_bstr_decode(previous, &ignored) ||
			    (name.len == earlier.len && !memcmp(name.value, earlier.value, name.len)) ||
			    prefix(&name, &earlier) || prefix(&earlier, &name)) {
				return -ENOEXEC;
			}
		}
		bool mpy = name.len > 4 && !memcmp(name.value + name.len - 4, ".mpy", 4);
		bool source = name.len > 3 && !memcmp(name.value + name.len - 3, ".py", 3);
		if (mpy || source) {
			int err = python_image_check(data.value, data.len,
				mpy ? APPLICATION_MPY : APPLICATION_PYTHON);
			if (err) {
				return err;
			}
		}
		entries += entry.len ? ((mpy || source) && name.len == entry.len &&
			!memcmp(name.value, entry.value, name.len)) :
			((name.len == 7 && !memcmp(name.value, "main.py", 7)) ||
			 (name.len == 8 && !memcmp(name.value, "main.mpy", 8)));
	}
	return entries == 1 && zcbor_map_end_decode(state) && zcbor_payload_at_end(state) ?
		0 : -ENOEXEC;
}

void python_files_set(const void *image, size_t length)
{
	files = image;
	files_size = length;
}

static mp_import_stat_t find(const char *path, struct zcbor_string *data)
{
	if (!files) {
		return MP_IMPORT_STAT_NO_EXIST;
	}
	if (*path == '/') {
		path++;
	}
	size_t length = strlen(path);
	if (!length) {
		return MP_IMPORT_STAT_DIR;
	}
	if (!path_valid(path, length)) {
		return MP_IMPORT_STAT_NO_EXIST;
	}
	struct zcbor_string wanted = {.value = (const uint8_t *)path, .len = length};
	ZCBOR_STATE_D(state, 2, files, files_size, 1, 0);
	if (!zcbor_map_start_decode(state)) {
		return MP_IMPORT_STAT_NO_EXIST;
	}
	for (size_t i = 0; i <= PYTHON_FILE_LIMIT && !zcbor_array_at_end(state); i++) {
		struct zcbor_string name, value;
		if (!zcbor_tstr_decode(state, &name) || !zcbor_bstr_decode(state, &value)) {
			break;
		}
		if (!name.len) {
			continue;
		}
		if (name.len == length && !memcmp(path, name.value, length)) {
			*data = value;
			return MP_IMPORT_STAT_FILE;
		}
		if (prefix(&wanted, &name)) {
			return MP_IMPORT_STAT_DIR;
		}
	}
	return MP_IMPORT_STAT_NO_EXIST;
}

mp_import_stat_t mp_import_stat(const char *path)
{
	struct zcbor_string data;
	return find(path, &data);
}

const char *python_files_entry(void)
{
	struct zcbor_string entry;
	if (files && explicit_entry(files, files_size, &entry) && entry.len) {
		return qstr_str(qstr_from_strn((const char *)entry.value, entry.len));
	}
	return mp_import_stat("main.py") == MP_IMPORT_STAT_FILE ? "main.py" : "main.mpy";
}

static struct zcbor_string read_file(qstr filename, enum application_format format)
{
	struct zcbor_string data;
	if (find(qstr_str(filename), &data) != MP_IMPORT_STAT_FILE) {
		mp_raise_OSError(ENOENT);
	}
	int err = python_image_check(data.value, data.len, format);
	if (err) {
		mp_raise_OSError(-err);
	}
	return data;
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
	struct zcbor_string data = read_file(filename, APPLICATION_PYTHON);
	return mp_lexer_new_from_str_len(filename, (const char *)data.value, data.len, 0);
}

struct bytecode_reader {
	const uint8_t *data;
	size_t remaining;
};

static mp_uint_t read_byte(void *context)
{
	struct bytecode_reader *reader = context;
	/* Returning EOF forever would hang upstream's varuint decoder on truncation. */
	if (!reader->remaining) {
		mp_raise_ValueError(MP_ERROR_TEXT("truncated .mpy"));
	}
	reader->remaining--;
	return *reader->data++;
}

static void close_reader(void *context)
{
	m_del_obj(struct bytecode_reader, context);
}

void python_reader_new_mem(mp_reader_t *reader, const void *data, size_t length)
{
	struct bytecode_reader *memory = m_new_obj(struct bytecode_reader);
	*memory = (struct bytecode_reader){.data = data, .remaining = length};
	*reader = (mp_reader_t){.data = memory, .readbyte = read_byte, .close = close_reader};
}

void mp_reader_new_file(mp_reader_t *reader, qstr filename)
{
	struct zcbor_string data = read_file(filename, APPLICATION_MPY);
	python_reader_new_mem(reader, data.value, data.len);
}

struct resource_file {
	mp_obj_base_t base;
	const uint8_t *data;
	size_t length, offset;
};

static mp_uint_t resource_read(mp_obj_t object, void *buffer, mp_uint_t size, int *error)
{
	struct resource_file *file = MP_OBJ_TO_PTR(object);
	if (!file->data) {
		*error = EBADF;
		return MP_STREAM_ERROR;
	}
	size = MIN(size, file->offset < file->length ? file->length - file->offset : 0);
	memcpy(buffer, file->data + MIN(file->offset, file->length), size);
	file->offset += size;
	return size;
}

static mp_uint_t resource_ioctl(mp_obj_t object, mp_uint_t request, uintptr_t arg, int *error)
{
	struct resource_file *file = MP_OBJ_TO_PTR(object);
	if (request == MP_STREAM_CLOSE) {
		file->data = NULL;
		return 0;
	}
	if (!file->data) {
		*error = EBADF;
		return MP_STREAM_ERROR;
	}
	if (request == MP_STREAM_SEEK) {
		struct mp_stream_seek_t *seek = (void *)arg;
		mp_off_t base;
		switch (seek->whence) {
		case MP_SEEK_SET: base = 0; break;
		case MP_SEEK_CUR: base = file->offset; break;
		case MP_SEEK_END: base = file->length; break;
		default: goto invalid;
		}
		if (seek->offset < -base || seek->offset > LONG_MAX - base) {
			goto invalid;
		}
		file->offset = seek->offset += base;
		return 0;
	}
	if (request == MP_STREAM_POLL) {
		return arg & MP_STREAM_POLL_RD;
	}
	if (request == MP_STREAM_FLUSH) {
		return 0;
	}
invalid:
	*error = EINVAL;
	return MP_STREAM_ERROR;
}

static const mp_rom_map_elem_t resource_methods[] = {
	{ MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
	{ MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
	{ MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
	{ MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
	{ MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
	{ MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
	{ MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
	{ MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
	{ MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(resource_dict, resource_methods);
static const mp_stream_p_t resource_binary = {.read = resource_read, .ioctl = resource_ioctl};
static const mp_stream_p_t resource_text = {
	.read = resource_read, .ioctl = resource_ioctl, .is_text = true,
};
static MP_DEFINE_CONST_OBJ_TYPE(resource_binary_type, MP_QSTR_FileIO, MP_TYPE_FLAG_ITER_IS_STREAM,
	protocol, &resource_binary, locals_dict, &resource_dict);
static MP_DEFINE_CONST_OBJ_TYPE(resource_text_type, MP_QSTR_TextIOWrapper, MP_TYPE_FLAG_ITER_IS_STREAM,
	protocol, &resource_text, locals_dict, &resource_dict);

static mp_obj_t resource_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
	static const mp_arg_t allowed[] = {
		{ MP_QSTR_file, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
		{ MP_QSTR_mode, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_r)} },
	};
	mp_arg_val_t values[MP_ARRAY_SIZE(allowed)];
	mp_arg_parse_all(n_args, args, kwargs, MP_ARRAY_SIZE(allowed), allowed, values);
	size_t mode_size, name_size;
	const char *mode = mp_obj_str_get_data(values[1].u_obj, &mode_size);
	const char *name = mp_obj_str_get_data(values[0].u_obj, &name_size);
	if (memchr(mode, 'w', mode_size) || memchr(mode, 'a', mode_size) ||
	    memchr(mode, 'x', mode_size) || memchr(mode, '+', mode_size)) {
		mp_raise_OSError(EROFS);
	}
	if ((mode_size != 1 && mode_size != 2) || mode[0] != 'r' ||
	    (mode_size == 2 && mode[1] != 'b' && mode[1] != 't') || memchr(name, 0, name_size)) {
		mp_raise_ValueError(MP_ERROR_TEXT("invalid file name or mode"));
	}
	struct zcbor_string data;
	mp_import_stat_t stat = find(name, &data);
	if (stat != MP_IMPORT_STAT_FILE) {
		mp_raise_OSError(stat == MP_IMPORT_STAT_DIR ? EISDIR : ENOENT);
	}
	struct resource_file *file = mp_obj_malloc(struct resource_file,
		mode_size == 2 && mode[1] == 'b' ? &resource_binary_type : &resource_text_type);
	file->data = data.value;
	file->length = data.len;
	file->offset = 0;
	return MP_OBJ_FROM_PTR(file);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, resource_open);
