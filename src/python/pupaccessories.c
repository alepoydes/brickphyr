// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
/* Adapted from pupdevices Light and ColorLightMatrix at a7b1b04ae.
 * Python parses values; shared port services own PWM, color encoding and I/O. */
#include "devices.h"
#include "py/mphal.h"
#include <brickphyr.h>
#include <errno.h>
#include <lego/device.h>
#include <pybricks/parameters.h>
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>

typedef struct {
	mp_obj_base_t base;
	pb_type_async_t *iter;
	uint32_t port, generation, request;
} accessory_obj_t;

static void connected(int error)
{
	python_device_check(error == -ESTALE ? -ENODEV : error);
}

static mp_obj_t light_write(accessory_obj_t *self, uint32_t brightness)
{
	connected(bp_port_light_write(self->port, self->generation, brightness, &self->request));
	for (;;) {
		struct bp_motor_state value;
		python_device_check(bp_motor_get(self->port, &value));
		if (value.generation != self->generation) python_device_check(-ENODEV);
		python_device_check(value.error);
		if (value.request != self->request) python_device_check(-ECANCELED);
		if (value.completed == self->request) return mp_const_none;
		mp_hal_delay_ms(2);
	}
}

static mp_obj_t accessory_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(port));
	bool light = type == &python_pup_light_type;
	struct bp_port_state value;
	uint32_t port = python_port_connect(port_in, light ? LEGO_DEVICE_TYPE_ID_LPF2_LIGHT :
		LEGO_DEVICE_TYPE_ID_TECHNIC_COLOR_LIGHT_MATRIX, &value);
	accessory_obj_t *self = mp_obj_malloc(accessory_obj_t, type);
	*self = (accessory_obj_t){.base = {.type = type}, .port = port, .generation = value.generation};
	if (light) light_write(self, 0);
	return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t light_on(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, accessory_obj_t, self, PB_ARG_DEFAULT_INT(brightness, 100));
	return light_write(self, pb_obj_get_pct(brightness_in) * 10);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(light_on_obj, 1, light_on);
static mp_obj_t light_off(mp_obj_t self) { return light_write(MP_OBJ_TO_PTR(self), 0); }
static MP_DEFINE_CONST_FUN_OBJ_1(light_off_obj, light_off);
static const mp_rom_map_elem_t light_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&light_on_obj)},
	{MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&light_off_obj)},
};
static MP_DEFINE_CONST_DICT(light_dict, light_methods);
MP_DEFINE_CONST_OBJ_TYPE(python_pup_light_type, MP_QSTR_Light, MP_TYPE_FLAG_NONE,
	make_new, accessory_new, locals_dict, &light_dict);

static pbio_error_t matrix_ready(pbio_os_state_t *state, mp_obj_t self_in)
{
	accessory_obj_t *self = MP_OBJ_TO_PTR(self_in);
	return python_port_write_done(self->port, self->generation, self->request) ? PBIO_SUCCESS : PBIO_ERROR_AGAIN;
}

static mp_obj_t matrix_write(mp_obj_t self_in, const uint32_t *colors)
{
	accessory_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (self->iter && self->iter->parent_obj != MP_OBJ_NULL) python_device_check(-EBUSY);
	connected(bp_port_matrix_write(self->port, self->generation, colors, &self->request));
	pb_type_async_t config = {.parent_obj = self_in, .iter_once = matrix_ready};
	if (pb_module_tools_run_loop_is_active()) {
		return pb_type_async_wait_or_await(&config, &self->iter, false);
	}
	while (matrix_ready(&config.state, self_in) == PBIO_ERROR_AGAIN) mp_hal_delay_ms(2);
	return mp_const_none;
}

static mp_obj_t matrix_on(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, accessory_obj_t, self, PB_ARG_REQUIRED(colors));
	uint32_t colors[9];
	if (mp_obj_is_type(colors_in, &pb_type_Color)) {
		uint32_t color = pb_type_Color_get_hsv(colors_in);
		for (size_t i = 0; i < MP_ARRAY_SIZE(colors); i++) colors[i] = color;
	} else {
		mp_obj_iter_buf_t buffer;
		mp_obj_t iterator = mp_getiter(colors_in, &buffer);
		for (size_t i = 0; i < MP_ARRAY_SIZE(colors); i++) {
			mp_obj_t color = mp_iternext(iterator);
			if (color == MP_OBJ_STOP_ITERATION) mp_raise_ValueError(MP_ERROR_TEXT("expected nine colors"));
			colors[i] = pb_type_Color_get_hsv(color);
		}
	}
	return matrix_write(MP_OBJ_FROM_PTR(self), colors);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_on_obj, 1, matrix_on);
static mp_obj_t matrix_off(mp_obj_t self) { return matrix_write(self, NULL); }
static MP_DEFINE_CONST_FUN_OBJ_1(matrix_off_obj, matrix_off);
static const mp_rom_map_elem_t matrix_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&matrix_on_obj)},
	{MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&matrix_off_obj)},
};
static MP_DEFINE_CONST_DICT(matrix_dict, matrix_methods);
MP_DEFINE_CONST_OBJ_TYPE(python_pup_matrix_type, MP_QSTR_ColorLightMatrix, MP_TYPE_FLAG_NONE,
	make_new, accessory_new, locals_dict, &matrix_dict);
