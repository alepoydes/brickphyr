// SPDX-License-Identifier: MIT
// Copyright (c) 2023 The Pybricks Authors
/* Broadcast codec adapted from pybricks/messaging/pb_type_ble_radio.c at a7b1b04ae.
 * Radio ownership and advertisement framing belong to the shared service. */
#include "devices.h"
#include "py/mphal.h"
#include <brickphyr.h>
#include <errno.h>
#include <pybricks/tools.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include <limits.h>
#include <string.h>
#include <pbio/util.h>
#include <pybricks/util_mp/pb_obj_helper.h>

#define BROADCAST_DATA_MAX BP_RADIO_DATA_MAX

enum broadcast_type {
	BROADCAST_SINGLE = 0,
	BROADCAST_TRUE = 1,
	BROADCAST_FALSE = 2,
	BROADCAST_INT = 3,
	BROADCAST_FLOAT = 4,
	BROADCAST_STR = 5,
	BROADCAST_BYTES = 6,
};

static void bad_broadcast(void)
{
	mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("received bad data"));
}

static mp_obj_t decode_value(const uint8_t *data, size_t size, size_t *index)
{
	if (*index >= size) bad_broadcast();
	uint8_t header = data[(*index)++];
	size_t length = header & 0x1f;
	enum broadcast_type type = header >> 5;
	if (length > size - *index) bad_broadcast();
	const uint8_t *value = data + *index;
	*index += length;
	switch (type) {
	case BROADCAST_TRUE:
	case BROADCAST_FALSE:
		if (length) bad_broadcast();
		return type == BROADCAST_TRUE ? mp_const_true : mp_const_false;
	case BROADCAST_INT:
		if (length == 1) return mp_obj_new_int((int8_t)value[0]);
		if (length == 2) return mp_obj_new_int((int16_t)pbio_get_uint16_le(value));
		if (length == 4) return mp_obj_new_int((int32_t)pbio_get_uint32_le(value));
		break;
	case BROADCAST_FLOAT: {
		if (length != 4) break;
		union { uint32_t bits; float number; } binary = {.bits = pbio_get_uint32_le(value)};
		return mp_obj_new_float_from_f(binary.number);
	}
	case BROADCAST_STR:
		return mp_obj_new_str((const char *)value, length);
	case BROADCAST_BYTES:
		return mp_obj_new_bytes(value, length);
	default:
		break;
	}
	bad_broadcast();
	MP_UNREACHABLE;
}

static mp_obj_t decode_broadcast(const uint8_t *data, size_t size)
{
	if (size > BROADCAST_DATA_MAX || (size && !data)) bad_broadcast();
	if (!size) return mp_obj_new_tuple(0, NULL);
	if ((data[0] >> 5) == BROADCAST_SINGLE) {
		if (data[0] != 0) bad_broadcast();
		size_t index = 1;
		mp_obj_t value = decode_value(data, size, &index);
		if (index != size) bad_broadcast();
		return value;
	}
	mp_obj_t values[BROADCAST_DATA_MAX];
	size_t count = 0, index = 0;
	while (index < size) values[count++] = decode_value(data, size, &index);
	return mp_obj_new_tuple(count, values);
}


typedef struct {
	mp_obj_base_t base;
	uint32_t handle;
} radio_obj_t;

static mp_obj_t radio_close(mp_obj_t self_in)
{
	radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (self->handle) {
		(void)bp_radio_close(self->handle);
		self->handle = 0;
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(radio_close_obj, radio_close);

static mp_obj_t radio_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_DEFAULT_NONE(broadcast_channel),
		PB_ARG_DEFAULT_OBJ(observe_channels, mp_const_empty_tuple_obj));
	pb_module_tools_assert_blocking();
	if (broadcast_channel_in != mp_const_none) {
		mp_raise_NotImplementedError(MP_ERROR_TEXT("radio broadcasting"));
	}
	mp_int_t count = mp_obj_get_int(mp_obj_len(observe_channels_in));
	if (count < 0 || count > BP_RADIO_CHANNELS_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("too many observe channels"));
	}
	uint8_t channels[BP_RADIO_CHANNELS_MAX];
	for (mp_int_t i = 0; i < count; i++) {
		mp_int_t channel = mp_obj_get_int(mp_obj_subscr(observe_channels_in,
			MP_OBJ_NEW_SMALL_INT(i), MP_OBJ_SENTINEL));
		if (channel < 0 || channel > UINT8_MAX) {
			mp_raise_ValueError(MP_ERROR_TEXT("observe channel must be 0 to 255"));
		}
		channels[i] = channel;
	}
	radio_obj_t *self = mp_obj_malloc_with_finaliser(radio_obj_t, type);
	self->handle = 0;
	python_device_check(bp_radio_observe(channels, count, &self->handle));
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		for (;;) {
			int err = bp_radio_ready(self->handle);
			if (err != -EAGAIN) {
				python_device_check(err);
				break;
			}
			mp_hal_delay_ms(2);
		}
		nlr_pop();
		return MP_OBJ_FROM_PTR(self);
	}
	radio_close(MP_OBJ_FROM_PTR(self));
	nlr_jump(nlr.ret_val);
}

static struct bp_radio_sample observed_sample(mp_obj_t self_in, mp_obj_t channel_in)
{
	radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (!self->handle) python_device_check(-EBADF);
	mp_int_t channel = mp_obj_get_int(channel_in);
	if (channel < 0 || channel > UINT8_MAX) mp_raise_ValueError(MP_ERROR_TEXT("channel not configured"));
	struct bp_radio_sample sample;
	int err = bp_radio_get(self->handle, channel, &sample);
	if (err == -EINVAL) mp_raise_ValueError(MP_ERROR_TEXT("channel not configured"));
	python_device_check(err);
	return sample;
}

static mp_obj_t radio_observe(mp_obj_t self_in, mp_obj_t channel_in)
{
	struct bp_radio_sample sample = observed_sample(self_in, channel_in);
	return sample.valid ? decode_broadcast(sample.data, sample.size) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(radio_observe_obj, radio_observe);

static mp_obj_t radio_strength(mp_obj_t self_in, mp_obj_t channel_in)
{
	struct bp_radio_sample sample = observed_sample(self_in, channel_in);
	return MP_OBJ_NEW_SMALL_INT(sample.rssi);
}
static MP_DEFINE_CONST_FUN_OBJ_2(radio_strength_obj, radio_strength);

static mp_obj_t radio_broadcast(mp_obj_t self, mp_obj_t data)
{
	mp_raise_NotImplementedError(MP_ERROR_TEXT("radio broadcasting"));
}
static MP_DEFINE_CONST_FUN_OBJ_2(radio_broadcast_obj, radio_broadcast);

static mp_obj_t radio_version(mp_obj_t self)
{
	char value[BP_RADIO_VERSION_MAX];
	python_device_check(bp_radio_version(value, sizeof(value)));
	return mp_obj_new_str(value, strlen(value));
}
static MP_DEFINE_CONST_FUN_OBJ_1(radio_version_obj, radio_version);

static const mp_rom_map_elem_t radio_methods[] = {
	{MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&radio_close_obj)},
	{MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&radio_close_obj)},
	{MP_ROM_QSTR(MP_QSTR_observe), MP_ROM_PTR(&radio_observe_obj)},
	{MP_ROM_QSTR(MP_QSTR_signal_strength), MP_ROM_PTR(&radio_strength_obj)},
	{MP_ROM_QSTR(MP_QSTR_broadcast), MP_ROM_PTR(&radio_broadcast_obj)},
	{MP_ROM_QSTR(MP_QSTR_version), MP_ROM_PTR(&radio_version_obj)},
};
static MP_DEFINE_CONST_DICT(radio_dict, radio_methods);

MP_DEFINE_CONST_OBJ_TYPE(pb_type_ble_radio, MP_QSTR_BLERadio, MP_TYPE_FLAG_NONE,
	make_new, radio_new, locals_dict, &radio_dict);

mp_obj_t pb_type_BLE_new(mp_obj_t broadcast, mp_obj_t channels)
{
	if (broadcast == mp_const_none && channels == MP_OBJ_FROM_PTR(&mp_const_empty_tuple_obj)) {
		return mp_const_none;
	}
	mp_obj_t args[] = {broadcast, channels};
	return radio_new(&pb_type_ble_radio, 2, 0, args);
}
