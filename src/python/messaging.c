// SPDX-License-Identifier: MIT
// Copyright (c) 2024 The Pybricks Authors
/* Adapted from pybricks/messaging/pb_type_app_data.c at a7b1b04ae.
 * The supervisor owns the RX buffer and copies outgoing messages. */
#include "devices.h"
#include "py/objstr.h"
#include "py/objlist.h"
#include "py/nlr.h"
#include <application_private.h>
#include <errno.h>
#include <string.h>
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>

typedef struct {
	uint8_t mode;
	size_t offset, size;
} app_data_mode_t;

typedef struct {
	mp_obj_base_t base;
	pb_type_async_t *tx_iter;
	uint32_t lease, request;
	size_t num_modes;
	app_data_mode_t modes[];
} app_data_obj_t;

static void check(int result) { python_device_check(result); }

static void opened(app_data_obj_t *self)
{
	if (!self->lease) check(-EBADF);
}

static app_data_mode_t *mode_info(app_data_obj_t *self, mp_obj_t mode_in)
{
	int mode = mp_obj_get_int(mode_in);
	for (size_t i = 0; i < self->num_modes; i++) {
		if (self->modes[i].mode == mode) return &self->modes[i];
	}
	mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
}

static mp_obj_t app_data_get_bytes(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, app_data_obj_t, self,
		PB_ARG_REQUIRED(mode), PB_ARG_DEFAULT_NONE(index));
	opened(self);
	app_data_mode_t *mode = mode_info(self, mode_in);
	if (index_in != mp_const_none) {
		mp_int_t index = mp_obj_get_int(index_in);
		if (index < 0 || (size_t)index >= mode->size) {
			mp_raise_ValueError(MP_ERROR_TEXT("index out of range"));
		}
		uint8_t value;
		check(application_message_read(self->lease, mode->offset + index, &value, 1));
		return mp_obj_new_int(value);
	}
	vstr_t bytes;
	vstr_init_len(&bytes, mode->size);
	check(application_message_read(self->lease, mode->offset, bytes.buf, mode->size));
	return mp_obj_new_bytes_from_vstr(&bytes);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(app_data_get_bytes_obj, 1, app_data_get_bytes);

static pbio_error_t write_next(pbio_os_state_t *state, mp_obj_t self_in)
{
	app_data_obj_t *self = MP_OBJ_TO_PTR(self_in);
	int result = application_message_status(self->lease, *state);
	if (result == -EAGAIN) return PBIO_ERROR_AGAIN;
	check(result);
	return PBIO_SUCCESS;
}

static mp_obj_t write_cancel(mp_obj_t self_in)
{
	app_data_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (self->lease && self->request) {
		/* The service compares both lease and request. A completed or replaced
		 * send cannot cancel a newer message. */
		(void)application_message_cancel(self->lease, self->request);
	}
	return mp_const_none;
}

static mp_obj_t send(app_data_obj_t *self, const uint8_t *data, size_t size)
{
	opened(self);
	if (size > APPLICATION_MESSAGE_TX_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("app data message too large"));
	}
	int request = application_message_send(self->lease, data, size);
	check(request);
	pb_type_async_schedule_stop_iteration(self->tx_iter);
	self->request = request;
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		pb_type_async_t config = {.parent_obj = MP_OBJ_FROM_PTR(self),
			.iter_once = write_next, .close = write_cancel, .state = request};
		mp_obj_t result = pb_type_async_wait_or_await(&config, &self->tx_iter, true);
		nlr_pop();
		return result;
	}
	/* Allocation failure or interruption must not leave a staged send behind. */
	(void)application_message_cancel(self->lease, request);
	nlr_jump(nlr.ret_val);
}

static mp_obj_t app_data_write_bytes(mp_obj_t self_in, mp_obj_t data_in)
{
	size_t size;
	const uint8_t *data = (const uint8_t *)mp_obj_str_get_data(data_in, &size);
	return send(MP_OBJ_TO_PTR(self_in), data, size);
}
static MP_DEFINE_CONST_FUN_OBJ_2(app_data_write_bytes_obj, app_data_write_bytes);

static mp_obj_t app_data_configure(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, app_data_obj_t, self,
		PB_ARG_REQUIRED(mode), PB_ARG_REQUIRED(parameter), PB_ARG_REQUIRED(value));
	size_t size;
	const uint8_t *value = (const uint8_t *)mp_obj_str_get_data(value_in, &size);
	if (size > APPLICATION_MESSAGE_TX_MAX - 3) {
		mp_raise_ValueError(MP_ERROR_TEXT("app data configuration too large"));
	}
	int mode = mp_obj_get_int(mode_in), parameter = mp_obj_get_int(parameter_in);
	if (mode < 0 || mode > UINT8_MAX || parameter < 0 || parameter > UINT8_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("invalid mode or parameter"));
	}
	vstr_t command;
	vstr_init_len(&command, 3 + size);
	command.buf[0] = 0x01;
	command.buf[1] = mode;
	command.buf[2] = parameter;
	memcpy(command.buf + 3, value, size);
	return app_data_write_bytes(MP_OBJ_FROM_PTR(self), mp_obj_new_bytes_from_vstr(&command));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(app_data_configure_obj, 1, app_data_configure);

static mp_obj_t app_data_close(mp_obj_t self_in)
{
	app_data_obj_t *self = MP_OBJ_TO_PTR(self_in);
	uint32_t lease = self->lease;
	self->lease = 0;
	self->request = 0;
	pb_type_async_schedule_stop_iteration(self->tx_iter);
	if (lease) (void)application_message_close(lease);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(app_data_close_obj, app_data_close);

static mp_obj_t app_data_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(modes));
	pb_module_tools_assert_blocking();
	pb_assert_type(modes_in, &mp_type_list);
	size_t num_modes;
	mp_obj_t *mode_items;
	mp_obj_get_array(modes_in, &num_modes, &mode_items);
	if (num_modes > APPLICATION_MESSAGE_TX_MAX - 1) {
		mp_raise_ValueError(MP_ERROR_TEXT("too many app data modes"));
	}
	mp_obj_t sorted = mp_obj_new_list(num_modes, mode_items);
	mp_obj_list_sort(1, &sorted, (mp_map_t *)&mp_const_empty_map);
	mp_obj_get_array(sorted, &num_modes, &mode_items);
	size_t capacity = 0;
	int previous = -1;
	for (size_t i = 0; i < num_modes; i++) {
		size_t pair_size;
		mp_obj_t *pair;
		mp_obj_get_array(mode_items[i], &pair_size, &pair);
		if (pair_size != 2) {
			mp_raise_TypeError(MP_ERROR_TEXT("modes must be a list of (mode, size) tuples"));
		}
		int mode = mp_obj_get_int(pair[0]);
		mp_int_t size = mp_obj_get_int(pair[1]);
		if (mode < 0 || mode > UINT8_MAX || mode == previous || size < 0 ||
		    (size_t)size > APPLICATION_MESSAGE_RX_MAX - capacity) {
			mp_raise_ValueError(MP_ERROR_TEXT("invalid app data mode or size"));
		}
		capacity += size;
		previous = mode;
	}
	vstr_t setup;
	vstr_init_len(&setup, 1 + num_modes);
	setup.buf[0] = 0x00;
	app_data_obj_t *self = mp_obj_malloc_var_with_finaliser(app_data_obj_t, modes,
		app_data_mode_t, num_modes, type);
	self->tx_iter = NULL;
	self->lease = self->request = 0;
	self->num_modes = num_modes;
	size_t offset = 0;
	for (size_t i = 0; i < num_modes; i++) {
		size_t pair_size;
		mp_obj_t *pair;
		mp_obj_get_array(mode_items[i], &pair_size, &pair);
		int mode = mp_obj_get_int(pair[0]);
		size_t size = mp_obj_get_int(pair[1]);
		self->modes[i] = (app_data_mode_t){.mode = mode, .offset = offset, .size = size};
		setup.buf[i + 1] = mode;
		offset += size;
	}
	int lease = application_message_open(capacity);
	check(lease);
	self->lease = lease;
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		(void)send(self, (const uint8_t *)setup.buf, setup.len);
		nlr_pop();
		return MP_OBJ_FROM_PTR(self);
	}
	/* Opening succeeded, but announcing modes did not. Leave no stale lease. */
	app_data_close(MP_OBJ_FROM_PTR(self));
	nlr_jump(nlr.ret_val);
}

static const mp_rom_map_elem_t app_data_methods[] = {
	{MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&app_data_close_obj)},
	{MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&app_data_close_obj)},
	{MP_ROM_QSTR(MP_QSTR_get_bytes), MP_ROM_PTR(&app_data_get_bytes_obj)},
	{MP_ROM_QSTR(MP_QSTR_write_bytes), MP_ROM_PTR(&app_data_write_bytes_obj)},
	{MP_ROM_QSTR(MP_QSTR_configure), MP_ROM_PTR(&app_data_configure_obj)},
};
static MP_DEFINE_CONST_DICT(app_data_dict, app_data_methods);

MP_DEFINE_CONST_OBJ_TYPE(pb_type_app_data, MP_QSTR_AppData, MP_TYPE_FLAG_NONE,
	make_new, app_data_new, locals_dict, &app_data_dict);
