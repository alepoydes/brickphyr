// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2024 The Pybricks Authors
/* Adapted from pybricks/iodevices/pb_type_iodevices_lwp3device.c at
 * a7b1b04ae. The protected radio service owns discovery, writes and samples. */
#include "devices.h"
#include <brickphyr.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <lego/lwp3.h>
#include <pbio/color.h>
#include <pbio/color_map.h>
#include <pybricks/common.h>
#include <pybricks/parameters.h>
#include <pybricks/parameters/pb_type_button.h>
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>
#include "py/mphal.h"
#include "py/nlr.h"
#include "py/objstr.h"

enum lwp3_operation { CONNECT = 1, DISCONNECT, WRITE };

typedef struct {
	mp_obj_base_t base;
	pb_type_async_t *iter;
	uint32_t handle;
	struct bp_lwp3_config config;
	mp_obj_t buttons, light;
	struct bp_sensor_palette palette;
} lwp3_obj_t;

static void release_handle(lwp3_obj_t *self)
{
	uint32_t handle = self->handle;
	self->handle = 0;
	if (handle) (void)bp_lwp3_close(handle);
}

static void retire(lwp3_obj_t *self)
{
	pb_type_async_schedule_stop_iteration(self->iter);
	release_handle(self);
}

static mp_obj_t lwp3_close(mp_obj_t self_in)
{
	retire(MP_OBJ_TO_PTR(self_in));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lwp3_close_obj, lwp3_close);

static void require_handle(lwp3_obj_t *self)
{
	if (!self->handle) python_device_check(-ENODEV);
}

static void get_state(lwp3_obj_t *self, struct bp_lwp3_state *state)
{
	require_handle(self);
	python_device_check(bp_lwp3_get(self->handle, state));
}

static pbio_error_t operation_ready(pbio_os_state_t *operation, mp_obj_t self_in)
{
	lwp3_obj_t *self = MP_OBJ_TO_PTR(self_in);
	struct bp_lwp3_state state;
	int err = self->handle ? bp_lwp3_get(self->handle, &state) : -ENODEV;
	if (err) {
		if (*operation != WRITE) release_handle(self);
		python_device_check(err);
	}
	if (state.busy) return PBIO_ERROR_AGAIN;
	err = state.error;
	if (*operation == DISCONNECT || (*operation == CONNECT && err)) release_handle(self);
	python_device_check(err);
	if (*operation == CONNECT && !state.connected) {
		release_handle(self);
		python_device_check(-ENOTCONN);
	}
	if (*operation != DISCONNECT) memcpy(self->config.name, state.name, sizeof(self->config.name));
	return PBIO_SUCCESS;
}

static mp_obj_t operation_cancel(mp_obj_t self_in)
{
	retire(MP_OBJ_TO_PTR(self_in));
	return mp_const_none;
}

static mp_obj_t await_operation(lwp3_obj_t *self, enum lwp3_operation operation)
{
	pb_type_async_schedule_stop_iteration(self->iter);
	pb_type_async_t config = {.parent_obj = MP_OBJ_FROM_PTR(self),
		.iter_once = operation_ready, .close = operation_cancel, .state = operation};
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
	/* Retain a completed write's connection after an ATT error, so the caller
	 * can retry. Interruption of pending work must still cancel its ownership. */
	struct bp_lwp3_state state;
	if (operation != WRITE || !self->handle || bp_lwp3_get(self->handle, &state) ||
	    state.busy || !state.connected) retire(self);
	nlr_jump(nlr.ret_val);
}

static mp_obj_t already_done(lwp3_obj_t *self)
{
	pb_type_async_schedule_stop_iteration(self->iter);
	return pb_type_async_return_result(mp_const_none, &self->iter);
}

static mp_obj_t lwp3_connect(mp_obj_t self_in)
{
	lwp3_obj_t *self = MP_OBJ_TO_PTR(self_in);
	bool opened = !self->handle;
	if (!self->handle) {
		uint32_t handle = 0;
		python_device_check(bp_lwp3_open(&self->config, &handle));
		self->handle = handle;
	}
	struct bp_lwp3_state state;
	int err = bp_lwp3_get(self->handle, &state);
	if (err || state.busy) {
		if (opened) retire(self);
		python_device_check(err ? err : -EBUSY);
	}
	if (self->config.profile == BP_LWP3_MARIO) {
		err = bp_lwp3_palette_set(self->handle, &self->palette);
	}
	if (!err) err = bp_lwp3_connect(self->handle);
	if (err) {
		retire(self);
		python_device_check(err);
	}
	return await_operation(self, CONNECT);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lwp3_connect_obj, lwp3_connect);

static mp_obj_t lwp3_disconnect(mp_obj_t self_in)
{
	lwp3_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (!self->handle) return already_done(self);
	struct bp_lwp3_state state;
	get_state(self, &state);
	if (!state.connected && !state.busy) {
		retire(self);
		return already_done(self);
	}
	python_device_check(bp_lwp3_disconnect(self->handle));
	return await_operation(self, DISCONNECT);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lwp3_disconnect_obj, lwp3_disconnect);

static mp_obj_t write_message(lwp3_obj_t *self, const uint8_t *data, size_t size)
{
	require_handle(self);
	python_device_check(bp_lwp3_write(self->handle, data, size));
	return await_operation(self, WRITE);
}

static mp_obj_t lwp3_name(size_t n_args, const mp_obj_t *args)
{
	lwp3_obj_t *self = MP_OBJ_TO_PTR(args[0]);
	if (n_args == 2) {
		size_t size;
		const char *name = mp_obj_str_get_data(args[1], &size);
		if (!size || size > BP_LWP3_NAME_MAX) {
			mp_raise_ValueError(MP_ERROR_TEXT("bad name length"));
		}
		uint8_t message[5 + BP_LWP3_NAME_MAX] = {
			5 + size, 0, LWP3_MSG_TYPE_HUB_PROPERTIES,
			LWP3_HUB_PROPERTY_NAME, LWP3_HUB_PROPERTY_OP_SET,
		};
		memcpy(message + 5, name, size);
		return write_message(self, message, 5 + size);
	}
	struct bp_lwp3_state state;
	get_state(self, &state);
	if (!state.connected) python_device_check(-ENODEV);
	return mp_obj_new_str(state.name, strnlen(state.name, BP_LWP3_NAME_MAX));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lwp3_name_obj, 1, 2, lwp3_name);

static mp_obj_t remote_pressed(mp_obj_t self_in)
{
	lwp3_obj_t *self = MP_OBJ_TO_PTR(self_in);
	struct bp_lwp3_state state;
	get_state(self, &state);
	if (!state.connected) python_device_check(-ENODEV);
	static const qstr names[] = {
		MP_QSTR_LEFT_PLUS, MP_QSTR_LEFT, MP_QSTR_LEFT_MINUS,
		MP_QSTR_RIGHT_PLUS, MP_QSTR_RIGHT, MP_QSTR_RIGHT_MINUS, MP_QSTR_CENTER,
	};
	mp_obj_t pressed[MP_ARRAY_SIZE(names)];
	size_t count = 0;
	for (size_t i = 0; i < MP_ARRAY_SIZE(names); i++) {
		if (state.buttons & (1U << i)) pressed[count++] = pb_type_button_new(names[i]);
	}
	#if MICROPY_PY_BUILTINS_SET
	return mp_obj_new_set(count, pressed);
	#else
	return mp_obj_new_tuple(count, pressed);
	#endif
}

static mp_obj_t device_command(lwp3_obj_t *self, uint32_t command, int32_t value, int32_t auxiliary)
{
	require_handle(self);
	python_device_check(bp_lwp3_command(self->handle, command, value, auxiliary));
	return await_operation(self, WRITE);
}

static mp_obj_t remote_light_on(mp_obj_t self_in, pbio_color_t hsv)
{
	return device_command(MP_OBJ_TO_PTR(self_in), BP_LWP3_LIGHT, hsv, 0);
}

static void set_config(lwp3_obj_t *self, mp_obj_t name_in, mp_obj_t timeout_in,
	mp_int_t kind, mp_int_t notifications, bool pair)
{
	if (kind < 0 || kind > UINT8_MAX || notifications < 0 || notifications > UINT8_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("invalid hub kind or notification count"));
	}
	self->config.kind = kind;
	self->config.notifications = notifications;
	self->config.flags = pair ? BP_LWP3_PAIR : 0;
	if (name_in != mp_const_none) {
		size_t size;
		const char *name = mp_obj_str_get_data(name_in, &size);
		if (size > BP_LWP3_NAME_MAX || memchr(name, 0, size)) {
			mp_raise_ValueError(MP_ERROR_TEXT("Name too long or contains NUL"));
		}
		memcpy(self->config.name, name, size);
	}
	if (timeout_in != mp_const_none) {
		mp_int_t timeout = mp_obj_get_int(timeout_in);
		if (timeout < 0 || timeout >= INT32_MAX) {
			mp_raise_ValueError(MP_ERROR_TEXT("invalid timeout"));
		}
		/* PBIO reserves zero for indefinite scanning and adds one to every
		 * finite timeout, including a requested zero milliseconds. */
		self->config.timeout_ms = timeout + 1;
	}
}

static mp_obj_t make_device(const mp_obj_type_t *type, mp_obj_t name_in,
	mp_obj_t timeout_in, mp_obj_t connect_in, mp_int_t kind, mp_int_t notifications, bool pair,
	uint8_t profile)
{
	pb_module_tools_assert_blocking();
	lwp3_obj_t *self = mp_obj_malloc_with_finaliser(lwp3_obj_t, type);
	self->iter = NULL;
	self->handle = 0;
	memset(&self->config, 0, sizeof(self->config));
	self->buttons = self->light = mp_const_none;
	set_config(self, name_in, timeout_in, kind, notifications, pair);
	self->config.profile = profile;
	if (profile == BP_LWP3_REMOTE) {
		self->buttons = pb_type_Keypad_obj_new(MP_OBJ_FROM_PTR(self), remote_pressed);
		self->light = pb_type_ColorLight_external_obj_new(MP_OBJ_FROM_PTR(self), remote_light_on);
	}
	if (profile == BP_LWP3_MARIO) {
		pbio_color_map_t map;
		pbio_color_map_set_default(&map);
		self->palette = (struct bp_sensor_palette){.count = map.num_colors};
		for (size_t i = 0; i < map.num_colors; i++) self->palette.colors[i] = map.colors[i];
	}
	bool connect = mp_obj_is_true(connect_in);
	uint32_t handle = 0;
	int err = bp_lwp3_open(&self->config, &handle);
	if (err) {
		/* A disconnected object need not own the sole peer if another class or
		 * name currently occupies it. connect() retries with this saved filter. */
		if (!connect && (err == -EBUSY || err == -EHOSTDOWN)) return MP_OBJ_FROM_PTR(self);
		python_device_check(err);
	}
	self->handle = handle;
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		if (connect) {
			(void)lwp3_connect(MP_OBJ_FROM_PTR(self));
		} else {
			struct bp_lwp3_state state;
			get_state(self, &state);
			if (state.connected) (void)lwp3_disconnect(MP_OBJ_FROM_PTR(self));
			else retire(self);
		}
		nlr_pop();
		return MP_OBJ_FROM_PTR(self);
	}
	retire(self);
	nlr_jump(nlr.ret_val);
}

static mp_obj_t profile_new(const mp_obj_type_t *type, size_t n_args,
	size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_DEFAULT_NONE(name),
		PB_ARG_DEFAULT_INT(timeout, 10000), PB_ARG_DEFAULT_TRUE(connect));
	uint8_t profile = type == &python_remote_type ? BP_LWP3_REMOTE :
		type == &python_technic_move_type ? BP_LWP3_TECHNIC_MOVE :
		type == &python_mario_type ? BP_LWP3_MARIO : BP_LWP3_DUPLO;
	return make_device(type, name_in, timeout_in, connect_in, 0, 0, false, profile);
}

static mp_obj_t lwp3_new(const mp_obj_type_t *type, size_t n_args,
	size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(hub_kind),
		PB_ARG_DEFAULT_NONE(name), PB_ARG_DEFAULT_INT(timeout, 10000),
		PB_ARG_DEFAULT_FALSE(pair), PB_ARG_DEFAULT_INT(num_notifications, 8),
		PB_ARG_DEFAULT_TRUE(connect));
	mp_int_t count = mp_obj_get_int(num_notifications_in);
	if (count < 0 || count > UINT8_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("invalid notification count"));
	}
	return make_device(type, name_in, timeout_in, connect_in,
		mp_obj_get_int(hub_kind_in), count ? count : 1, mp_obj_is_true(pair_in), BP_LWP3_RAW);
}

static mp_obj_t lwp3_write(mp_obj_t self_in, mp_obj_t data_in)
{
	mp_buffer_info_t data;
	mp_get_buffer_raise(data_in, &data, MP_BUFFER_READ);
	if (data.len < 3 || data.len > BP_LWP3_MESSAGE_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("bad length"));
	}
	if (((uint8_t *)data.buf)[0] != data.len) {
		mp_raise_ValueError(MP_ERROR_TEXT("length in header wrong"));
	}
	return write_message(MP_OBJ_TO_PTR(self_in), data.buf, data.len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(lwp3_write_obj, lwp3_write);

static mp_obj_t lwp3_read(mp_obj_t self_in)
{
	lwp3_obj_t *self = MP_OBJ_TO_PTR(self_in);
	require_handle(self);
	struct bp_lwp3_message message;
	int size = bp_lwp3_read(self->handle, &message);
	python_device_check(size);
	if (!size) return mp_const_none;
	if (size < 3 || size > BP_LWP3_MESSAGE_MAX || message.size != size) {
		mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("received bad data"));
	}
	return mp_obj_new_bytes(message.data, size);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lwp3_read_obj, lwp3_read);

static void sensor_state(lwp3_obj_t *self, struct bp_lwp3_state *state, uint8_t fields)
{
	get_state(self, state);
	if (!state->connected) python_device_check(-ENODEV);
	if (!(state->fields & fields)) python_device_check(-EAGAIN);
}

static mp_obj_t device_color(mp_obj_t self_in)
{
	struct bp_lwp3_state state;
	sensor_state(MP_OBJ_TO_PTR(self_in), &state, BP_LWP3_HAS_RGB | BP_LWP3_HAS_TAG);
	return state.fields & BP_LWP3_HAS_MATCHED_COLOR ? pb_type_Color_from_hsv(state.matched_color) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(device_color_obj, device_color);

static mp_obj_t mario_hsv(mp_obj_t self_in)
{
	struct bp_lwp3_state state;
	sensor_state(MP_OBJ_TO_PTR(self_in), &state, BP_LWP3_HAS_RGB);
	return pb_type_Color_new(state.hsv);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mario_hsv_obj, mario_hsv);

static mp_obj_t mario_colors(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, lwp3_obj_t, self, PB_ARG_DEFAULT_NONE(colors));
	if (colors_in == mp_const_none) {
		if (self->handle) python_device_check(bp_lwp3_palette_get(self->handle, &self->palette));
		mp_obj_t colors[BP_SENSOR_PALETTE_SIZE];
		for (size_t i = 0; i < self->palette.count; i++) colors[i] = pb_type_Color_from_hsv(self->palette.colors[i]);
		return mp_obj_new_tuple(self->palette.count, colors);
	}
	size_t count;
	mp_obj_t *colors;
	mp_obj_get_array(colors_in, &count, &colors);
	if (count > BP_SENSOR_PALETTE_SIZE) mp_raise_ValueError(MP_ERROR_TEXT("too many colors"));
	struct bp_sensor_palette palette = {.count = count};
	for (size_t i = 0; i < count; i++) palette.colors[i] = pb_type_Color_get_hsv(colors[i]);
	if (self->handle) python_device_check(bp_lwp3_palette_set(self->handle, &palette));
	self->palette = palette;
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mario_colors_obj, 1, mario_colors);

static mp_obj_t technic_drive(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, lwp3_obj_t, self,
		PB_ARG_REQUIRED(speed), PB_ARG_REQUIRED(steering));
	return device_command(self, BP_LWP3_DRIVE, pb_obj_get_int(speed_in), pb_obj_get_int(steering_in));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(technic_drive_obj, 1, technic_drive);

static mp_obj_t duplo_drive(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, lwp3_obj_t, self, PB_ARG_REQUIRED(power));
	return device_command(self, BP_LWP3_DRIVE, pb_obj_get_int(power_in), 0);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(duplo_drive_obj, 1, duplo_drive);

static mp_obj_t duplo_headlights(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, lwp3_obj_t, self, PB_ARG_REQUIRED(color));
	return device_command(self, BP_LWP3_LIGHT, pb_type_Color_get_hsv(color_in), 0);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(duplo_headlights_obj, 1, duplo_headlights);

static mp_obj_t duplo_sound(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, lwp3_obj_t, self, PB_ARG_REQUIRED(sound));
	uint32_t sound;
	switch (mp_obj_str_get_qstr(sound_in)) {
	case MP_QSTR_brake: sound = BP_LWP3_BRAKE; break;
	case MP_QSTR_depart: sound = BP_LWP3_DEPART; break;
	case MP_QSTR_water: sound = BP_LWP3_WATER; break;
	case MP_QSTR_horn: sound = BP_LWP3_HORN; break;
	case MP_QSTR_steam: sound = BP_LWP3_STEAM; break;
	default: mp_raise_ValueError(MP_ERROR_TEXT("unknown train sound"));
	}
	return device_command(self, BP_LWP3_SOUND, sound, 0);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(duplo_sound_obj, 1, duplo_sound);

static mp_obj_t duplo_speed(mp_obj_t self_in)
{
	struct bp_lwp3_state state;
	sensor_state(MP_OBJ_TO_PTR(self_in), &state, BP_LWP3_HAS_SPEED);
	return mp_obj_new_int(state.speed);
}
static MP_DEFINE_CONST_FUN_OBJ_1(duplo_speed_obj, duplo_speed);

static const pb_attr_dict_entry_t remote_attrs[] = {
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_buttons, lwp3_obj_t, buttons),
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_light, lwp3_obj_t, light),
	PB_ATTR_DICT_SENTINEL,
};

static const mp_rom_map_elem_t remote_methods[] = {
	{MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&lwp3_close_obj)},
	{MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&lwp3_connect_obj)},
	{MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&lwp3_disconnect_obj)},
	{MP_ROM_QSTR(MP_QSTR_name), MP_ROM_PTR(&lwp3_name_obj)},
};
static MP_DEFINE_CONST_DICT(remote_dict, remote_methods);

MP_DEFINE_CONST_OBJ_TYPE(python_remote_type, MP_QSTR_Remote, MP_TYPE_FLAG_NONE,
	make_new, profile_new, attr, pb_attribute_handler, protocol, remote_attrs,
	locals_dict, &remote_dict);

static const mp_rom_map_elem_t lwp3_methods[] = {
	{MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&lwp3_close_obj)},
	{MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&lwp3_connect_obj)},
	{MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&lwp3_disconnect_obj)},
	{MP_ROM_QSTR(MP_QSTR_name), MP_ROM_PTR(&lwp3_name_obj)},
	{MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&lwp3_write_obj)},
	{MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&lwp3_read_obj)},
};
static MP_DEFINE_CONST_DICT(lwp3_dict, lwp3_methods);

MP_DEFINE_CONST_OBJ_TYPE(python_lwp3device_type, MP_QSTR_LWP3Device, MP_TYPE_FLAG_NONE,
	make_new, lwp3_new, locals_dict, &lwp3_dict);

static const mp_rom_map_elem_t technic_methods[] = {
	{MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&lwp3_close_obj)},
	{MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&lwp3_connect_obj)},
	{MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&lwp3_disconnect_obj)},
	{MP_ROM_QSTR(MP_QSTR_drive), MP_ROM_PTR(&technic_drive_obj)},
};
static MP_DEFINE_CONST_DICT(technic_dict, technic_methods);
MP_DEFINE_CONST_OBJ_TYPE(python_technic_move_type, MP_QSTR_TechnicMoveHub, MP_TYPE_FLAG_NONE,
	make_new, profile_new, locals_dict, &technic_dict);

static const mp_rom_map_elem_t mario_methods[] = {
	{MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&lwp3_close_obj)},
	{MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&lwp3_connect_obj)},
	{MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&lwp3_disconnect_obj)},
	{MP_ROM_QSTR(MP_QSTR_color), MP_ROM_PTR(&device_color_obj)},
	{MP_ROM_QSTR(MP_QSTR_hsv), MP_ROM_PTR(&mario_hsv_obj)},
	{MP_ROM_QSTR(MP_QSTR_detectable_colors), MP_ROM_PTR(&mario_colors_obj)},
};
static MP_DEFINE_CONST_DICT(mario_dict, mario_methods);
MP_DEFINE_CONST_OBJ_TYPE(python_mario_type, MP_QSTR_MarioHub, MP_TYPE_FLAG_NONE,
	make_new, profile_new, locals_dict, &mario_dict);

static const mp_rom_map_elem_t duplo_methods[] = {
	{MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&lwp3_close_obj)},
	{MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&lwp3_connect_obj)},
	{MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&lwp3_disconnect_obj)},
	{MP_ROM_QSTR(MP_QSTR_drive), MP_ROM_PTR(&duplo_drive_obj)},
	{MP_ROM_QSTR(MP_QSTR_headlights), MP_ROM_PTR(&duplo_headlights_obj)},
	{MP_ROM_QSTR(MP_QSTR_sound), MP_ROM_PTR(&duplo_sound_obj)},
	{MP_ROM_QSTR(MP_QSTR_speed), MP_ROM_PTR(&duplo_speed_obj)},
	{MP_ROM_QSTR(MP_QSTR_color), MP_ROM_PTR(&device_color_obj)},
};
static MP_DEFINE_CONST_DICT(duplo_dict, duplo_methods);
MP_DEFINE_CONST_OBJ_TYPE(python_duplo_type, MP_QSTR_DuploTrain, MP_TYPE_FLAG_NONE,
	make_new, profile_new, locals_dict, &duplo_dict);
