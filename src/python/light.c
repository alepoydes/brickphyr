// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
// Adapted from pybricks/common/pb_type_colorlight_internal.c.
#include "devices.h"
#include "py/mphal.h"
#include <brickphyr.h>
#include <errno.h>
#include <string.h>
#include <pybricks/parameters.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>

typedef struct {
	mp_obj_base_t base;
	uint32_t light;
} light_obj_t;

static void wait_applied(light_obj_t *self)
{
	struct bp_light_state state;
	do {
		python_device_check(bp_light_get(self->light, &state));
		if (state.pending) {
			mp_hal_delay_ms(1);
		}
	} while (state.pending);
	python_device_check(state.error);
}

static void color_rgb(mp_obj_t color, uint8_t channels[3])
{
	pbio_color_rgb_t rgb;
	pbio_color_to_rgb(pb_type_Color_get_hsv(color), &rgb);
	channels[0] = rgb.r;
	channels[1] = rgb.g;
	channels[2] = rgb.b;
}

static void set_color(light_obj_t *self, const uint8_t channels[3])
{
	int err;
	while ((err = bp_light_write(self->light, channels)) == -EBUSY) {
		mp_hal_delay_ms(1);
	}
	python_device_check(err);
	wait_applied(self);
}

static mp_obj_t light_on(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, light_obj_t, self, PB_ARG_REQUIRED(color));
	uint8_t rgb[3];
	color_rgb(color_in, rgb);
	set_color(self, rgb);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(light_on_obj, 1, light_on);

static mp_obj_t light_off(mp_obj_t self_in)
{
	const uint8_t rgb[3] = {0};
	set_color(MP_OBJ_TO_PTR(self_in), rgb);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(light_off_obj, light_off);

static size_t frame_count(mp_obj_t sequence)
{
	mp_int_t count = mp_obj_get_int(mp_obj_len(sequence));
	if (count <= 0 || count > BP_LIGHT_MAX_FRAMES) {
		mp_raise_ValueError(MP_ERROR_TEXT("expected 1..256 light frames"));
	}
	return count;
}

static uint32_t duration(mp_obj_t value)
{
	mp_int_t milliseconds = pb_obj_get_int(value);
	if (milliseconds <= 0 || milliseconds > INT32_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("duration must be 1..2147483647 ms"));
	}
	return milliseconds;
}

static mp_obj_t animate(light_obj_t *self, struct bp_light_frame *frames, size_t count)
{
	int err;
	while ((err = bp_light_animate(self->light, frames, count)) == -EBUSY) {
		mp_hal_delay_ms(1);
	}
	m_del(struct bp_light_frame, frames, count);
	python_device_check(err);
	wait_applied(self);
	return mp_const_none;
}

static mp_obj_t light_blink(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, light_obj_t, self,
		PB_ARG_REQUIRED(color), PB_ARG_REQUIRED(durations));
	uint8_t rgb[3];
	color_rgb(color_in, rgb);
	size_t count = frame_count(durations_in);
	struct bp_light_frame *frames = m_new(struct bp_light_frame, count);
	mp_obj_iter_buf_t buffer;
	mp_obj_t iterator = mp_getiter(durations_in, &buffer);
	for (size_t i = 0; i < count; i++) {
		frames[i] = (struct bp_light_frame){.duration_ms = duration(mp_iternext(iterator))};
		if (!(i % 2)) {
			memcpy(frames[i].rgb, rgb, sizeof(rgb));
		}
	}
	return animate(self, frames, count);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(light_blink_obj, 1, light_blink);

static mp_obj_t light_animate(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, light_obj_t, self,
		PB_ARG_REQUIRED(colors), PB_ARG_REQUIRED(interval));
	uint32_t milliseconds = duration(interval_in);
	size_t count = frame_count(colors_in);
	struct bp_light_frame *frames = m_new(struct bp_light_frame, count);
	mp_obj_iter_buf_t buffer;
	mp_obj_t iterator = mp_getiter(colors_in, &buffer);
	for (size_t i = 0; i < count; i++) {
		frames[i] = (struct bp_light_frame){.duration_ms = milliseconds};
		color_rgb(mp_iternext(iterator), frames[i].rgb);
	}
	return animate(self, frames, count);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(light_animate_obj, 1, light_animate);

static const mp_rom_map_elem_t light_methods[] = {
	{ MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&light_on_obj) },
	{ MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&light_off_obj) },
	{ MP_ROM_QSTR(MP_QSTR_blink), MP_ROM_PTR(&light_blink_obj) },
	{ MP_ROM_QSTR(MP_QSTR_animate), MP_ROM_PTR(&light_animate_obj) },
};
static MP_DEFINE_CONST_DICT(light_dict, light_methods);
static MP_DEFINE_CONST_OBJ_TYPE(light_type, MP_QSTR_ColorLight, MP_TYPE_FLAG_NONE, locals_dict, &light_dict);

mp_obj_t python_light_new(uint32_t light)
{
	light_obj_t *self = mp_obj_malloc(light_obj_t, &light_type);
	self->light = light;
	return MP_OBJ_FROM_PTR(self);
}
