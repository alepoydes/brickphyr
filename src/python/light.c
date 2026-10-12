// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
// Adapted from pybricks/common/pb_type_colorlight_internal.c.
#include "devices.h"
#include "py/mphal.h"
#include <brickphyr.h>
#include <errno.h>
#include <pybricks/parameters.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>

typedef struct {
	mp_obj_base_t base;
	uint32_t light;
} light_obj_t;

static void set_color(light_obj_t *self, pbio_color_t color)
{
	pbio_color_rgb_t rgb;
	pbio_color_to_rgb(color, &rgb);
	const uint8_t channels[] = {rgb.r, rgb.g, rgb.b};
	int err;
	while ((err = bp_light_write(self->light, channels)) == -EBUSY) {
		mp_hal_delay_ms(1);
	}
	python_device_check(err);
	struct bp_light_state state;
	do {
		python_device_check(bp_light_get(self->light, &state));
		if (state.pending) {
			mp_hal_delay_ms(1);
		}
	} while (state.pending);
	python_device_check(state.error);
}

static mp_obj_t light_on(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, light_obj_t, self, PB_ARG_REQUIRED(color));
	set_color(self, pb_type_Color_get_hsv(color_in));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(light_on_obj, 1, light_on);

static mp_obj_t light_off(mp_obj_t self_in)
{
	set_color(MP_OBJ_TO_PTR(self_in), PBIO_COLOR_ENCODE(0, 0, 0));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(light_off_obj, light_off);

static const mp_rom_map_elem_t light_methods[] = {
	{ MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&light_on_obj) },
	{ MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&light_off_obj) },
};
static MP_DEFINE_CONST_DICT(light_dict, light_methods);
static MP_DEFINE_CONST_OBJ_TYPE(light_type, MP_QSTR_ColorLight, MP_TYPE_FLAG_NONE, locals_dict, &light_dict);

mp_obj_t python_light_new(uint32_t light)
{
	light_obj_t *self = mp_obj_malloc(light_obj_t, &light_type);
	self->light = light;
	return MP_OBJ_FROM_PTR(self);
}
