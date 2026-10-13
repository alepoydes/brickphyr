// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Pybricks Authors
/* Adapted from pybricks/messaging/pb_type_hub_network.c at a7b1b04ae.
 * Brickphyr's protected network service owns the links, inboxes, relay queue,
 * and copies each outgoing message before this binding yields. */
#include "devices.h"
#include <brickphyr.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include "py/mphal.h"
#include "py/nlr.h"
#include "py/objarray.h"
#include "py/objlist.h"
#include "py/objstr.h"
#include "py/objtuple.h"
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>

#if !MICROPY_PY_STRUCT
#error "HubNetwork messages need the struct module"
#endif

#define ADDRESS_STR_SIZE 17
#define CONNECT_ATTEMPTS 3
#define CONNECT_DRAIN_MS 5000
#define DEFAULT_INBOX_SIZE 1024
#define FORMAT_SIZE_MASK 0x7f
#define FORMAT_TUPLE_FLAG 0x80
#define FORMAT_INT 'i'
#define FORMAT_FLOAT 'f'
#define FORMAT_BOOL '?'
#define FORMAT_NONE 'n'
#define FORMAT_BYTES 's'
#define FORMAT_STR 'u'
#define MAX_MESSAGE_OBJECTS 32
#define FORMAT_BUFFER_SIZE (MAX_MESSAGE_OBJECTS * 4 + 2)

enum network_operation { NETWORK_CONNECT, NETWORK_SEND };

typedef struct {
	mp_obj_base_t base;
	pb_type_async_t *iter;
	uint32_t handle, request;
	uint8_t operation, attempts;
	uint8_t connect_address[6];
	uint32_t retry_started;
	struct bp_network_message message;
	mp_obj_t pack_into, unpack_from;
	mp_obj_array_t tx_view, rx_view;
	mp_obj_str_t tx_format_obj, rx_format_obj;
	char tx_format[FORMAT_BUFFER_SIZE], rx_format[FORMAT_BUFFER_SIZE];
} network_obj_t;

static void require_open(network_obj_t *self)
{
	if (!self->handle) python_device_check(-EBADF);
}

/* The address and typed wire format below retain the Pybricks codec. */
static mp_obj_t address_to_obj(const uint8_t *address)
{
	static const char digits[] = "0123456789ABCDEF";
	char value[ADDRESS_STR_SIZE];
	for (size_t i = 0; i < 6; i++) {
		value[i * 3] = digits[address[i] >> 4];
		value[i * 3 + 1] = digits[address[i] & 15];
		if (i != 5) value[i * 3 + 2] = ':';
	}
	return mp_obj_new_str(value, sizeof(value));
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void address_from_obj(mp_obj_t obj, uint8_t *address)
{
	size_t len;
	const char *value = mp_obj_str_get_data(obj, &len);
	if (len != ADDRESS_STR_SIZE) goto invalid;
	for (size_t i = 0; i < 6; i++) {
		int high = hex_digit(value[i * 3]);
		int low = hex_digit(value[i * 3 + 1]);
		if (high < 0 || low < 0 || (i != 5 && value[i * 3 + 2] != ':')) goto invalid;
		address[i] = high << 4 | low;
	}
	return;
invalid:
	mp_raise_ValueError(MP_ERROR_TEXT("expected an address like '00:16:53:12:34:56'"));
}

static uint32_t format_write_size(uint8_t *format, uint32_t value)
{
	uint32_t size = 0;
	if (value >= 100) format[size++] = '0' + value / 100;
	if (value >= 10) format[size++] = '0' + value / 10 % 10;
	format[size++] = '0' + value % 10;
	return size;
}

static uint32_t format_object(mp_obj_t obj, uint8_t *format, uint32_t *payload)
{
	if (obj == mp_const_none) {
		*payload += 1;
		format[0] = FORMAT_NONE;
		return 1;
	}
	if (mp_obj_is_bool(obj)) {
		*payload += 1;
		format[0] = FORMAT_BOOL;
		return 1;
	}
	if (mp_obj_is_int(obj)) {
		mp_int_t value = mp_obj_get_int(obj);
		if ((int32_t)value != value) {
			mp_raise_msg(&mp_type_OverflowError, MP_ERROR_TEXT("int is too big to send"));
		}
		*payload += 4;
		format[0] = FORMAT_INT;
		return 1;
	}
#if MICROPY_PY_BUILTINS_FLOAT
	if (mp_obj_is_float(obj)) {
		*payload += 4;
		format[0] = FORMAT_FLOAT;
		return 1;
	}
#endif
	mp_buffer_info_t info;
	if (mp_get_buffer(obj, &info, MP_BUFFER_READ)) {
		if (info.len > UINT8_MAX) mp_raise_ValueError(MP_ERROR_TEXT("message is too big to send"));
		uint32_t size = format_write_size(format, info.len);
		format[size++] = mp_obj_is_str(obj) ? FORMAT_STR : FORMAT_BYTES;
		*payload += info.len;
		return size;
	}
	mp_raise_TypeError(MP_ERROR_TEXT("cannot send this object"));
}

static void format_to_struct(const uint8_t *format, uint32_t size, char *dest)
{
	bool counted = false;
	dest[0] = '<';
	for (uint32_t i = 0; i < size; i++) {
		char c = format[i];
		bool digit = c >= '0' && c <= '9';
		switch (c) {
		case FORMAT_STR: c = FORMAT_BYTES; break;
		case FORMAT_BYTES: break;
		case FORMAT_BOOL:
		case FORMAT_NONE: c = 'B'; MP_FALLTHROUGH
		case FORMAT_INT:
		case FORMAT_FLOAT:
			if (counted) mp_raise_ValueError(MP_ERROR_TEXT("received a bad message"));
			break;
		default:
			if (!digit) mp_raise_ValueError(MP_ERROR_TEXT("received a bad message"));
			break;
		}
		counted = digit;
		dest[1 + i] = c;
	}
	if (counted) mp_raise_ValueError(MP_ERROR_TEXT("received a bad message"));
	dest[1 + size] = '\0';
}

static void message_encode(network_obj_t *self, mp_obj_t input)
{
	uint8_t *buffer = self->message.data;
	self->message.size = 0;
	mp_buffer_info_t info;
	if (!mp_obj_is_str(input) && mp_get_buffer(input, &info, MP_BUFFER_READ)) {
		if (info.len > BP_NETWORK_DATA_MAX - 1) {
			mp_raise_ValueError(MP_ERROR_TEXT("message is too big to send"));
		}
		buffer[0] = 0;
		memcpy(buffer + 1, info.buf, info.len);
		self->message.size = info.len + 1;
		return;
	}
	size_t count = 1;
	mp_obj_t *objects = &input;
	bool single = !mp_obj_is_type(input, &mp_type_tuple);
	if (!single) mp_obj_tuple_get(input, &count, &objects);
	if (count > MAX_MESSAGE_OBJECTS) mp_raise_ValueError(MP_ERROR_TEXT("message has too many objects"));
	uint32_t format_size = 0, payload_size = 0;
	mp_obj_t args[MAX_MESSAGE_OBJECTS + 3];
	for (size_t i = 0; i < count; i++) {
		format_size += format_object(objects[i], &buffer[1 + format_size], &payload_size);
		args[3 + i] = objects[i] == mp_const_none ? MP_OBJ_NEW_SMALL_INT(0) : objects[i];
	}
	if (format_size > FORMAT_SIZE_MASK || 1 + format_size + payload_size > BP_NETWORK_DATA_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("message is too big to send"));
	}
	buffer[0] = format_size | (single ? 0 : FORMAT_TUPLE_FLAG);
	format_to_struct(buffer + 1, format_size, self->tx_format);
	self->tx_format_obj.len = 1 + format_size;
	args[0] = MP_OBJ_FROM_PTR(&self->tx_format_obj);
	args[1] = MP_OBJ_FROM_PTR(&self->tx_view);
	args[2] = MP_OBJ_NEW_SMALL_INT(1 + format_size);
	mp_call_function_n_kw(self->pack_into, 3 + count, 0, args);
	self->message.size = 1 + format_size + payload_size;
}

static mp_obj_t message_decode(network_obj_t *self)
{
	uint32_t size = self->message.size;
	uint8_t *buffer = self->message.data;
	if (!size || size > BP_NETWORK_DATA_MAX) goto invalid;
	uint32_t header = buffer[0], format_size = header & FORMAT_SIZE_MASK;
	if (!header) return mp_obj_new_bytes(buffer + 1, size - 1);
	if (1 + format_size > size) goto invalid;
	format_to_struct(buffer + 1, format_size, self->rx_format);
	self->rx_format_obj.len = 1 + format_size;
	self->rx_view.len = size;
	mp_obj_t args[] = { MP_OBJ_FROM_PTR(&self->rx_format_obj),
		MP_OBJ_FROM_PTR(&self->rx_view), MP_OBJ_NEW_SMALL_INT(1 + format_size) };
	mp_obj_t result = mp_call_function_n_kw(self->unpack_from, MP_ARRAY_SIZE(args), 0, args);
	size_t count;
	mp_obj_t *objects;
	mp_obj_tuple_get(result, &count, &objects);
	for (uint32_t i = 0, object = 0; i < format_size && object < count; i++) {
		uint8_t c = buffer[1 + i];
		if (c == FORMAT_STR) {
			size_t len;
			const char *str = mp_obj_str_get_data(objects[object], &len);
			objects[object] = mp_obj_new_str(str, len);
		} else if (c == FORMAT_BOOL) {
			objects[object] = mp_obj_new_bool(mp_obj_get_int(objects[object]));
		} else if (c == FORMAT_NONE) {
			objects[object] = mp_const_none;
		}
		object += !(c >= '0' && c <= '9');
	}
	if (header & FORMAT_TUPLE_FLAG) return result;
	if (count != 1) goto invalid;
	return objects[0];
invalid:
	mp_raise_ValueError(MP_ERROR_TEXT("received a bad message"));
}

static mp_obj_t network_close(mp_obj_t self_in)
{
	network_obj_t *self = MP_OBJ_TO_PTR(self_in);
	uint32_t handle = self->handle;
	self->handle = self->request = 0;
	pb_type_async_schedule_stop_iteration(self->iter);
	if (handle) (void)bp_network_close(handle);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_close_obj, network_close);

static mp_obj_t operation_cancel(mp_obj_t self_in)
{
	network_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (self->handle && self->request) (void)bp_network_cancel(self->handle, self->request);
	return mp_const_none;
}

static bool permanent_connect_error(int error)
{
	return error == -ENOMEM || error == -ENOBUFS || error == -ENOSPC ||
		error == -EBUSY || error == -ENODEV || error == -ENOTSUP ||
		error == -EINVAL || error == -EBADF ||
		error == -ESHUTDOWN || error == -EACCES;
}

static pbio_error_t operation_ready(pbio_os_state_t *token, mp_obj_t self_in)
{
	network_obj_t *self = MP_OBJ_TO_PTR(self_in);
	/* A failed page keeps its peer slot until the radio worker observes the
	 * disconnect callback. Do not count that drain time as another attempt. */
	if (self->operation == NETWORK_CONNECT && !*token) {
		int next = bp_network_connect(self->handle, self->connect_address);
		if (next == -EAGAIN) {
			if ((uint32_t)(mp_hal_ticks_ms() - self->retry_started) >= CONNECT_DRAIN_MS) {
				python_device_check(-ETIMEDOUT);
			}
			return PBIO_ERROR_AGAIN;
		}
		if (next < 0) {
			if (permanent_connect_error(next)) python_device_check(next);
			mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("could not connect to brick"));
		}
		self->request = *token = next;
		self->attempts++;
		return PBIO_ERROR_AGAIN;
	}
	struct bp_network_state state;
	python_device_check(bp_network_get(self->handle, &state));
	if (state.request != *token) python_device_check(-ECANCELED);
	if (state.busy) return PBIO_ERROR_AGAIN;
	if (self->operation == NETWORK_CONNECT && state.error) {
		if (self->attempts < CONNECT_ATTEMPTS && !permanent_connect_error(state.error)) {
			self->request = *token = 0;
			self->retry_started = mp_hal_ticks_ms();
			return PBIO_ERROR_AGAIN;
		}
		mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("could not connect to brick"));
	}
	python_device_check(state.error);
	return PBIO_SUCCESS;
}

static mp_obj_t await_operation(network_obj_t *self, uint32_t token)
{
	self->request = token;
	if (!token) self->retry_started = mp_hal_ticks_ms();
	pb_type_async_t config = {.parent_obj = MP_OBJ_FROM_PTR(self),
		.iter_once = operation_ready, .close = operation_cancel, .state = token};
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		mp_obj_t result;
		if (pb_module_tools_run_loop_is_active()) {
			result = pb_type_async_wait_or_await(&config, &self->iter, true);
		} else {
			while (operation_ready(&config.state, config.parent_obj) == PBIO_ERROR_AGAIN) {
				mp_hal_delay_ms(2);
			}
			result = mp_const_none;
		}
		nlr_pop();
		return result;
	}
	(void)bp_network_cancel(self->handle, self->request);
	nlr_jump(nlr.ret_val);
}

static void replace_operation(network_obj_t *self)
{
	if (self->request) (void)bp_network_cancel(self->handle, self->request);
	pb_type_async_schedule_stop_iteration(self->iter);
	self->request = 0;
}

static mp_obj_t network_address(mp_obj_t self_in)
{
	network_obj_t *self = MP_OBJ_TO_PTR(self_in);
	require_open(self);
	struct bp_network_state state;
	python_device_check(bp_network_get(self->handle, &state));
	return address_to_obj(state.address);
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_address_obj, network_address);

static mp_obj_t network_connect(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, network_obj_t, self, PB_ARG_REQUIRED(address));
	require_open(self);
	struct bp_network_state state;
	python_device_check(bp_network_get(self->handle, &state));
	uint8_t address[6];
	address_from_obj(address_in, address);
	if (!memcmp(address, state.address, sizeof(address))) {
		mp_raise_ValueError(MP_ERROR_TEXT("cannot connect to this brick itself"));
	}
	replace_operation(self);
	int token = bp_network_connect(self->handle, address);
	if (token != -EAGAIN) python_device_check(token);
	memcpy(self->connect_address, address, sizeof(address));
	self->operation = NETWORK_CONNECT;
	self->attempts = token > 0 ? 1 : 0;
	return await_operation(self, token > 0 ? token : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_connect_obj, 1, network_connect);

static mp_obj_t network_is_connected(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, network_obj_t, self, PB_ARG_DEFAULT_NONE(address));
	require_open(self);
	struct bp_network_state state;
	python_device_check(bp_network_get(self->handle, &state));
	if (address_in == mp_const_none) return mp_obj_new_bool(state.count != 0);
	uint8_t address[6];
	address_from_obj(address_in, address);
	for (size_t i = 0; i < state.count && i < BP_NETWORK_PEERS; i++) {
		if (!memcmp(address, state.peers[i], sizeof(address))) return mp_const_true;
	}
	return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_is_connected_obj, 1, network_is_connected);

static mp_obj_t network_send(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, network_obj_t, self,
		PB_ARG_REQUIRED(data), PB_ARG_DEFAULT_NONE(address));
	require_open(self);
	uint8_t address[6];
	if (address_in == mp_const_none) memset(address, 0xff, sizeof(address));
	else address_from_obj(address_in, address);
	message_encode(self, data_in);
	memcpy(self->message.address, address, sizeof(address));
	replace_operation(self);
	int token = bp_network_send(self->handle, &self->message);
	python_device_check(token);
	self->operation = NETWORK_SEND;
	return await_operation(self, token);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_send_obj, 1, network_send);

static mp_obj_t network_inbox(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, network_obj_t, self, PB_ARG_DEFAULT_FALSE(latest));
	require_open(self);
	uint32_t through = 0;
	python_device_check(bp_network_snapshot(self->handle, mp_obj_is_true(latest_in), &through));
	mp_obj_t messages = mp_obj_new_list(0, NULL);
	for (;;) {
		int result = bp_network_read(self->handle, through, &self->message);
		python_device_check(result);
		if (!result) break;
		mp_obj_t item[] = {address_to_obj(self->message.address), message_decode(self)};
		mp_obj_list_append(messages, mp_obj_new_tuple(MP_ARRAY_SIZE(item), item));
	}
	size_t len;
	mp_obj_t *items;
	mp_obj_list_get(messages, &len, &items);
	return mp_obj_new_tuple(len, items);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_inbox_obj, 1, network_inbox);

static mp_obj_t network_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_DEFAULT_INT(inbox_size, DEFAULT_INBOX_SIZE));
	pb_module_tools_assert_blocking();
	mp_int_t inbox_size = pb_obj_get_positive_int(inbox_size_in);
	if (inbox_size < BP_NETWORK_INBOX_MIN || (uint64_t)inbox_size > UINT32_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("inbox_size is too small for one message"));
	}
	network_obj_t *self = mp_obj_malloc_with_finaliser(network_obj_t, type);
	self->iter = NULL;
	self->handle = self->request = 0;
	self->pack_into = pb_function_import_helper(MP_QSTR_struct, MP_QSTR_pack_into);
	self->unpack_from = pb_function_import_helper(MP_QSTR_struct, MP_QSTR_unpack_from);
	mp_obj_memoryview_init(&self->tx_view, 'B' | MP_OBJ_ARRAY_TYPECODE_FLAG_RW, 0,
		BP_NETWORK_DATA_MAX, self->message.data);
	mp_obj_memoryview_init(&self->rx_view, 'B', 0,
		BP_NETWORK_DATA_MAX, self->message.data);
	self->tx_format_obj.base.type = &mp_type_str;
	self->tx_format_obj.hash = 0;
	self->tx_format_obj.data = (const byte *)self->tx_format;
	self->rx_format_obj.base.type = &mp_type_str;
	self->rx_format_obj.hash = 0;
	self->rx_format_obj.data = (const byte *)self->rx_format;
	uint32_t handle;
	int result;
	do {
		result = bp_network_open(inbox_size, &handle);
		if (result == -EAGAIN) mp_hal_delay_ms(2);
	} while (result == -EAGAIN);
	python_device_check(result);
	self->handle = handle;
	return MP_OBJ_FROM_PTR(self);
}

static const mp_rom_map_elem_t network_methods[] = {
	{MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&network_close_obj)},
	{MP_ROM_QSTR(MP_QSTR_address), MP_ROM_PTR(&network_address_obj)},
	{MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&network_connect_obj)},
	{MP_ROM_QSTR(MP_QSTR_is_connected), MP_ROM_PTR(&network_is_connected_obj)},
	{MP_ROM_QSTR(MP_QSTR_inbox), MP_ROM_PTR(&network_inbox_obj)},
	{MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&network_send_obj)},
};
static MP_DEFINE_CONST_DICT(network_dict, network_methods);

MP_DEFINE_CONST_OBJ_TYPE(pb_type_hub_network, MP_QSTR_HubNetwork, MP_TYPE_FLAG_NONE,
	make_new, network_new, locals_dict, &network_dict);
