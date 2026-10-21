// SPDX-License-Identifier: Apache-2.0 AND MIT
#include "files.h"
#include "python_runtime.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <zephyr/sys/util.h>
#include "app_package.h"
#include "py/builtin.h"
#include "py/lexer.h"
#include "py/runtime.h"
#include "py/stream.h"

/* The read-only stream below adapts MicroPython extmod/vfs_rom_file.c,
 * Copyright (c) 2022 Damien P. George, MIT. Storage lookup remains this port's
 * existing program namespace; no VFS or second filesystem is instantiated. */

/* The package owner outlives every open reader. Only offsets cross the
 * application boundary; buffers contain bounded copies owned by Python. */
static struct app_source files;

int python_files_check(const void *image, size_t length)
{
	const struct app_source source = {app_source_memory, (void *)image, length};
	return python_source_check(&source);
}

void python_files_set_source(const struct app_source *source)
{
	files = source ? *source : (struct app_source){0};
}

static mp_import_stat_t find(const char *path, struct app_resource *file)
{
	if (!files.read) return MP_IMPORT_STAT_NO_EXIST;
	int type = app_package_find(&files, path, file);
	if (type < 0) mp_raise_OSError(-type);
	return type == APP_RESOURCE_FILE ? MP_IMPORT_STAT_FILE :
		type == APP_RESOURCE_DIR ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_NO_EXIST;
}

mp_import_stat_t mp_import_stat(const char *path)
{
	struct app_resource file;
	return find(path, &file);
}

const char *python_files_entry(void)
{
	char path[APP_PATH_LIMIT + 1];
	int err = app_package_entry(&files, path, sizeof(path));
	if (err) mp_raise_OSError(-err);
	return qstr_str(qstr_from_str(path));
}

struct app_resource python_files_resource(const char *path)
{
	struct app_resource file;
	mp_import_stat_t type = find(path, &file);
	if (type != MP_IMPORT_STAT_FILE) mp_raise_OSError(type == MP_IMPORT_STAT_DIR ? EISDIR : ENOENT);
	return file;
}

struct source_reader {
	struct app_source source;
	struct app_resource file;
	size_t offset, cursor, available;
	bool bytecode;
	uint8_t buffer[128];
};

static mp_uint_t read_byte(void *context)
{
	struct source_reader *reader = context;
	if (reader->cursor == reader->available) {
		if (reader->offset == reader->file.length) {
			/* Upstream's varuint decoder does not terminate on a missing byte. */
			if (reader->bytecode) mp_raise_ValueError(MP_ERROR_TEXT("truncated .mpy"));
			return MP_READER_EOF;
		}
		size_t size = MIN(sizeof(reader->buffer), reader->file.length - reader->offset);
		int err = app_source_read(&reader->source, reader->file.offset + reader->offset, reader->buffer, size);
		if (err) mp_raise_OSError(-err);
		reader->offset += size;
		reader->cursor = 0;
		reader->available = size;
	}
	return reader->buffer[reader->cursor++];
}

static void close_reader(void *context)
{
	m_del_obj(struct source_reader, context);
}

void python_reader_new_source(mp_reader_t *reader, const struct app_source *source,
	struct app_resource file, bool bytecode)
{
	struct source_reader *stream = m_new_obj(struct source_reader);
	*stream = (struct source_reader){.source = *source, .file = file, .bytecode = bytecode};
	*reader = (mp_reader_t){.data = stream, .readbyte = read_byte, .close = close_reader};
}

void python_reader_new_mem(mp_reader_t *reader, const void *data, size_t length)
{
	const struct app_source source = {app_source_memory, (void *)data, length};
	python_reader_new_source(reader, &source, (struct app_resource){0, length}, true);
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
	mp_reader_t reader;
	python_reader_new_source(&reader, &files, python_files_resource(qstr_str(filename)), false);
	return mp_lexer_new(filename, reader);
}

void mp_reader_new_file(mp_reader_t *reader, qstr filename)
{
	struct app_resource file = python_files_resource(qstr_str(filename));
	uint8_t header[4];
	int err = file.length < sizeof(header) ? -ENOEXEC : app_source_read(&files, file.offset, header, sizeof(header));
	if (!err) err = python_image_check(header, sizeof(header), APPLICATION_MPY);
	if (err) mp_raise_OSError(-err);
	python_reader_new_source(reader, &files, file, true);
}

struct resource_file {
	mp_obj_base_t base;
	struct app_resource resource;
	size_t offset;
	bool closed;
};

static mp_uint_t resource_read(mp_obj_t object, void *buffer, mp_uint_t size, int *error)
{
	struct resource_file *file = MP_OBJ_TO_PTR(object);
	if (file->closed) {
		*error = EBADF;
		return MP_STREAM_ERROR;
	}
	size = MIN(size, file->offset < file->resource.length ? file->resource.length - file->offset : 0);
	int err = size ? app_source_read(&files, file->resource.offset + file->offset, buffer, size) : 0;
	if (err) {
		*error = -err;
		return MP_STREAM_ERROR;
	}
	file->offset += size;
	return size;
}

static mp_uint_t resource_ioctl(mp_obj_t object, mp_uint_t request, uintptr_t arg, int *error)
{
	struct resource_file *file = MP_OBJ_TO_PTR(object);
	if (request == MP_STREAM_CLOSE) {
		file->closed = true;
		return 0;
	}
	if (file->closed) {
		*error = EBADF;
		return MP_STREAM_ERROR;
	}
	if (request == MP_STREAM_SEEK) {
		struct mp_stream_seek_t *seek = (void *)arg;
		mp_off_t base;
		switch (seek->whence) {
		case MP_SEEK_SET: base = 0; break;
		case MP_SEEK_CUR: base = file->offset; break;
		case MP_SEEK_END: base = file->resource.length; break;
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
	struct app_resource data;
	mp_import_stat_t stat = find(name, &data);
	if (stat != MP_IMPORT_STAT_FILE) {
		mp_raise_OSError(stat == MP_IMPORT_STAT_DIR ? EISDIR : ENOENT);
	}
	struct resource_file *file = mp_obj_malloc(struct resource_file,
		mode_size == 2 && mode[1] == 'b' ? &resource_binary_type : &resource_text_type);
	file->resource = data;
	file->offset = 0;
	file->closed = false;
	return MP_OBJ_FROM_PTR(file);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, resource_open);
