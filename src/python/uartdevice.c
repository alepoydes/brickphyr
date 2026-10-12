// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2025 The Pybricks Authors
/* Adapted from pybricks/iodevices/pb_type_uart_device.c at a7b1b04ae.
 * The shared port worker replaces PBIO I/O. No Python buffer enters an ISR. */
#include "devices.h"
#include "py/mphal.h"
#include "py/objarray.h"
#include <brickphyr.h>
#include <errno.h>
#include <string.h>
#include <pybricks/parameters.h>
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>
#include <pybricks/util_mp/pb_type_enum.h>

typedef struct {
	mp_obj_base_t base;
	uint32_t port, generation, timeout, request;
	pb_type_async_t *read_iter, *write_iter;
	mp_obj_str_t *read_obj;
	mp_obj_t write_obj, pattern;
	size_t read_pos, write_pos, matched;
	size_t *prefix;
	int64_t read_deadline, write_deadline;
} uart_obj_t;

static void check(int err) { python_device_check(err == -ESTALE ? -ENODEV : err); }

static void configured(uart_obj_t *self)
{
	for (;;) {
		struct bp_port_state value;
		check(bp_port_get(self->port, &value));
		if (value.generation != self->generation) check(-ENODEV);
		if (value.error != -EAGAIN) {
			check(value.error);
			return;
		}
		mp_hal_delay_ms(2);
	}
}

static uint32_t positive(mp_obj_t value)
{
	mp_int_t n = mp_obj_get_int(value);
	if (n <= 0) mp_raise_ValueError(MP_ERROR_TEXT("expected positive integer"));
	return n;
}

static mp_obj_t uart_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(port),
		PB_ARG_DEFAULT_INT(baudrate, 115200), PB_ARG_DEFAULT_NONE(timeout), PB_ARG_DEFAULT_INT(power_pin, 0));
	pb_module_tools_assert_blocking();
	int port = pb_type_enum_get_value(port_in, &pb_enum_type_Port) - PBIO_PORT_ID_A;
	if (port < 0 || port >= BP_PORT_COUNT) mp_raise_ValueError(MP_ERROR_TEXT("expected port A-F"));
	uint32_t baud = positive(baudrate_in);
	uint32_t timeout = timeout_in == mp_const_none ? 0 : positive(timeout_in);
	mp_int_t power = mp_obj_get_int(power_pin_in);
	if (power < 0 || power > 2) mp_raise_ValueError(MP_ERROR_TEXT("invalid power pin"));
	/* Custom supply permission/persistence is not yet part of this profile.
	 * Refuse before claiming a port or touching its outputs. */
	if (power) check(-ENOTSUP);
	uart_obj_t *self = mp_obj_malloc(uart_obj_t, type);
	*self = (uart_obj_t){.base = {.type = type}, .port = port, .timeout = timeout};
	check(bp_uart_open(port, baud, &self->generation));
	configured(self);
	return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t wait(uart_obj_t *self, bool reading, pb_type_async_iterate_once_t iter_once,
	pb_type_async_return_map_t return_map)
{
	pb_type_async_t config = {.parent_obj = MP_OBJ_FROM_PTR(self), .iter_once = iter_once, .return_map = return_map};
	pb_type_async_t **iter = reading ? &self->read_iter : &self->write_iter;
	if (pb_module_tools_run_loop_is_active()) return pb_type_async_wait_or_await(&config, iter, true);
	pb_type_async_schedule_stop_iteration(*iter);
	while (iter_once(&config.state, config.parent_obj) == PBIO_ERROR_AGAIN) mp_hal_delay_ms(2);
	return return_map ? return_map(config.parent_obj) : mp_const_none;
}

static void timed_out(int64_t deadline)
{
	if (deadline && bp_time_ms() >= deadline) check(-ETIMEDOUT);
}

static pbio_error_t read_ready(pbio_os_state_t *state, mp_obj_t self_in)
{
	uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
	size_t left = self->read_obj->len - self->read_pos;
	if (!left) return PBIO_SUCCESS;
	int count = bp_uart_read(self->port, self->generation,
		(uint8_t *)self->read_obj->data + self->read_pos, MIN(left, 128));
	check(count);
	self->read_pos += count;
	if (self->read_pos == self->read_obj->len) return PBIO_SUCCESS;
	timed_out(self->read_deadline);
	return PBIO_ERROR_AGAIN;
}

static mp_obj_t read_result(mp_obj_t self_in)
{
	uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
	mp_obj_t result = pb_obj_new_bytes_finish(self->read_obj);
	self->read_obj = NULL;
	return result;
}

static mp_obj_t uart_read(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, uart_obj_t, self, PB_ARG_DEFAULT_INT(length, 1));
	mp_obj_str_t *data = pb_obj_new_bytes_prepare(pb_obj_get_positive_int(length_in));
	self->read_obj = data;
	self->read_pos = 0;
	self->pattern = MP_OBJ_NULL;
	self->prefix = NULL;
	self->read_deadline = self->timeout ? bp_time_ms() + self->timeout : 0;
	return wait(self, true, read_ready, read_result);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(uart_read_obj, 1, uart_read);

static mp_obj_t uart_waiting(mp_obj_t self_in)
{
	uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
	int count = bp_uart_waiting(self->port, self->generation);
	check(count);
	return mp_obj_new_int(count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(uart_waiting_obj, uart_waiting);

static mp_obj_t uart_read_all(mp_obj_t self_in)
{
	uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
	int size = bp_uart_waiting(self->port, self->generation);
	check(size);
	mp_obj_str_t *data = pb_obj_new_bytes_prepare(size);
	pb_type_async_schedule_stop_iteration(self->read_iter);
	for (int pos = 0; pos < size;) {
		int count = bp_uart_read(self->port, self->generation, (uint8_t *)data->data + pos, MIN(size - pos, 128));
		check(count);
		if (!count) check(-EIO); /* Only this application consumes the RX ring. */
		pos += count;
	}
	self->read_obj = NULL;
	self->pattern = MP_OBJ_NULL;
	self->prefix = NULL;
	return pb_obj_new_bytes_finish(data);
}
static MP_DEFINE_CONST_FUN_OBJ_1(uart_read_all_obj, uart_read_all);

static pbio_error_t write_ready(pbio_os_state_t *state, mp_obj_t self_in)
{
	uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (self->request) {
		if (!python_port_write_done(self->port, self->generation, self->request)) {
			timed_out(self->write_deadline);
			return PBIO_ERROR_AGAIN;
		}
		self->request = 0;
	}
	size_t size;
	const uint8_t *data = mp_obj_str_get_data(self->write_obj, &size);
	if (self->write_pos == size) {
		self->write_obj = MP_OBJ_NULL;
		return PBIO_SUCCESS;
	}
	timed_out(self->write_deadline);
	size_t count = MIN(size - self->write_pos, 32);
	check(bp_uart_write(self->port, self->generation, data + self->write_pos, count, &self->request));
	self->write_pos += count;
	return PBIO_ERROR_AGAIN;
}

static mp_obj_t uart_write(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, uart_obj_t, self, PB_ARG_REQUIRED(data));
	mp_obj_t data = data_in;
	if (mp_obj_is_type(data, &mp_type_bytearray)) {
		mp_buffer_info_t buffer;
		mp_get_buffer_raise(data, &buffer, MP_BUFFER_READ);
		data = mp_obj_new_bytes(buffer.buf, buffer.len);
	} else if (!mp_obj_is_str_or_bytes(data)) mp_raise_TypeError(MP_ERROR_TEXT("expected str, bytes or bytearray"));
	/* Immutable snapshot: a bytearray may be resized while an async write yields. */
	self->write_obj = data;
	self->write_pos = 0;
	self->write_deadline = self->timeout ? bp_time_ms() + self->timeout : 0;
	return wait(self, false, write_ready, NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(uart_write_obj, 1, uart_write);

static pbio_error_t pattern_ready(pbio_os_state_t *state, mp_obj_t self_in)
{
	uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
	size_t size;
	const uint8_t *pattern = mp_obj_str_get_data(self->pattern, &size);
	/* Stop exactly after the pattern; never consume its trailing payload.
	 * Prefix matching preserves overlapping prefixes across RX chunks. */
	for (size_t n = 0; n < 128; n++) {
		uint8_t byte;
		int count = bp_uart_read(self->port, self->generation, &byte, 1);
		check(count);
		if (!count) break;
		while (self->matched && pattern[self->matched] != byte) self->matched = self->prefix[self->matched - 1];
		if (pattern[self->matched] == byte) self->matched++;
		if (self->matched == size) {
			self->pattern = MP_OBJ_NULL;
			self->prefix = NULL;
			return PBIO_SUCCESS;
		}
	}
	return PBIO_ERROR_AGAIN;
}

static mp_obj_t uart_wait_until(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, uart_obj_t, self, PB_ARG_REQUIRED(pattern));
	size_t size;
	const uint8_t *data = mp_obj_str_get_data(pattern_in, &size);
	if (!size) mp_raise_ValueError(MP_ERROR_TEXT("pattern must not be empty"));
	size_t *prefix = m_new(size_t, size);
	prefix[0] = 0;
	for (size_t i = 1, match = 0; i < size; i++) {
		while (match && data[i] != data[match]) match = prefix[match - 1];
		if (data[i] == data[match]) match++;
		prefix[i] = match;
	}
	self->pattern = pattern_in;
	self->prefix = prefix;
	self->matched = 0;
	self->read_obj = NULL;
	return wait(self, true, pattern_ready, NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(uart_wait_until_obj, 1, uart_wait_until);

static mp_obj_t reconfigure(uart_obj_t *self, uint32_t baud)
{
	check(baud ? bp_uart_configure(self->port, self->generation, baud) : bp_uart_clear(self->port, self->generation));
	pb_type_async_schedule_stop_iteration(self->read_iter);
	pb_type_async_schedule_stop_iteration(self->write_iter);
	self->read_obj = NULL;
	self->write_obj = self->pattern = MP_OBJ_NULL;
	self->prefix = NULL;
	self->request = 0;
	configured(self);
	return mp_const_none;
}
static mp_obj_t uart_clear(mp_obj_t self_in) { return reconfigure(MP_OBJ_TO_PTR(self_in), 0); }
static MP_DEFINE_CONST_FUN_OBJ_1(uart_clear_obj, uart_clear);
static mp_obj_t uart_baud(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, uart_obj_t, self, PB_ARG_REQUIRED(baudrate));
	return reconfigure(self, positive(baudrate_in));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(uart_baud_obj, 1, uart_baud);

static const mp_rom_map_elem_t uart_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&uart_read_obj)},
	{MP_ROM_QSTR(MP_QSTR_read_all), MP_ROM_PTR(&uart_read_all_obj)},
	{MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&uart_write_obj)},
	{MP_ROM_QSTR(MP_QSTR_waiting), MP_ROM_PTR(&uart_waiting_obj)},
	{MP_ROM_QSTR(MP_QSTR_wait_until), MP_ROM_PTR(&uart_wait_until_obj)},
	{MP_ROM_QSTR(MP_QSTR_set_baudrate), MP_ROM_PTR(&uart_baud_obj)},
	{MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&uart_clear_obj)},
};
static MP_DEFINE_CONST_DICT(uart_dict, uart_methods);
MP_DEFINE_CONST_OBJ_TYPE(python_uart_type, MP_QSTR_UARTDevice, MP_TYPE_FLAG_NONE,
	make_new, uart_new, locals_dict, &uart_dict);
