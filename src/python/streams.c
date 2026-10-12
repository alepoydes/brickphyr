// SPDX-License-Identifier: MIT
// Copyright (c) 2013-2019 Damien P. George
// Adapted from shared/runtime/sys_stdio_mphal.c: keep stream direction for
// .buffer and poll; flush has no VM-side buffer to drain.
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/stream.h"

struct stdio {
	mp_obj_base_t base;
	unsigned int fd;
};

static mp_uint_t read_stdin(mp_obj_t object, void *buffer, mp_uint_t size, int *error)
{
	struct stdio *self = MP_OBJ_TO_PTR(object);
	if (self->fd) {
		*error = MP_EPERM;
		return MP_STREAM_ERROR;
	}
	const mp_stream_p_t *stream = mp_get_stream(object);
	for (size_t i = 0; i < size; i++) {
		int c = mp_hal_stdin_rx_chr();
		((uint8_t *)buffer)[i] = stream->is_text && c == '\r' ? '\n' : c;
	}
	return size;
}

static mp_uint_t write_stdout(mp_obj_t object, const void *buffer, mp_uint_t size, int *error)
{
	struct stdio *self = MP_OBJ_TO_PTR(object);
	if (!self->fd) {
		*error = MP_EPERM;
		return MP_STREAM_ERROR;
	}
	if (mp_get_stream(object)->is_text) {
		mp_hal_stdout_tx_strn_cooked(buffer, size);
	} else {
		mp_hal_stdout_tx_strn(buffer, size);
	}
	/* Attached output applies backpressure in the VM's interruptible I/O loop;
	 * detached output uses the same queue as a nonblocking, counted log. */
	return size;
}

static mp_uint_t stdio_ioctl(mp_obj_t object, mp_uint_t request, uintptr_t arg, int *error)
{
	struct stdio *self = MP_OBJ_TO_PTR(object);
	switch (request) {
	case MP_STREAM_POLL:
		return mp_hal_stdio_poll(arg & (self->fd ? MP_STREAM_POLL_WR : MP_STREAM_POLL_RD));
	case MP_STREAM_FLUSH:
	case MP_STREAM_CLOSE:
		return 0;
	default:
		*error = MP_EINVAL;
		return MP_STREAM_ERROR;
	}
}

static const mp_rom_map_elem_t stdio_methods[] = {
	{ MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
	{ MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
	{ MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
	{ MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
	{ MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
	{ MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
	{ MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
	{ MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
	{ MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(stdio_dict, stdio_methods);
static const mp_stream_p_t binary_stream = {
	.read = read_stdin, .write = write_stdout, .ioctl = stdio_ioctl,
};
static MP_DEFINE_CONST_OBJ_TYPE(binary_type, MP_QSTR_FileIO, MP_TYPE_FLAG_ITER_IS_STREAM,
	protocol, &binary_stream, locals_dict, &stdio_dict);
static const struct stdio binary_stdio[] = {
	{{&binary_type}, 0}, {{&binary_type}, 1}, {{&binary_type}, 2},
};

static void stdio_attr(mp_obj_t object, qstr name, mp_obj_t *dest)
{
	struct stdio *self = MP_OBJ_TO_PTR(object);
	if (dest[0] == MP_OBJ_NULL) {
		if (name == MP_QSTR_buffer) {
			dest[0] = MP_OBJ_FROM_PTR(&binary_stdio[self->fd]);
		} else {
			dest[1] = MP_OBJ_SENTINEL;
		}
	}
}

static const mp_stream_p_t text_stream = {
	.read = read_stdin, .write = write_stdout, .ioctl = stdio_ioctl, .is_text = true,
};
static MP_DEFINE_CONST_OBJ_TYPE(text_type, MP_QSTR_TextIOWrapper, MP_TYPE_FLAG_ITER_IS_STREAM,
	attr, stdio_attr, protocol, &text_stream, locals_dict, &stdio_dict);
const struct stdio mp_sys_stdin_obj = {{&text_type}, 0};
const struct stdio mp_sys_stdout_obj = {{&text_type}, 1};
const struct stdio mp_sys_stderr_obj = {{&text_type}, 2};
