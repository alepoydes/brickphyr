// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
// Adapted from pybricks/common/pb_type_lightmatrix.c and PBIO light/light_matrix.c.
#include "devices.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include <brickphyr.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <pybricks/common.h>
#include <pybricks/parameters.h>
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_matrix.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>
#include <pybricks/util_mp/pb_type_enum.h>

typedef struct {
	mp_obj_base_t base;
	pb_type_async_t *text_iter;
	mp_obj_t text;
	size_t text_index;
	int64_t deadline;
	uint32_t on, off;
	bool blank, pending;
} matrix_obj_t;

static void stop_text(matrix_obj_t *self)
{
	pb_type_async_schedule_stop_iteration(self->text_iter);
	self->text = MP_OBJ_NULL;
}

static void wait_applied(void)
{
	struct bp_matrix_state state;
	do {
		python_device_check(bp_matrix_get(&state));
		if (state.pending) {
			mp_hal_delay_ms(1);
		}
	} while (state.pending);
	python_device_check(state.error);
}

static void wait_available(void)
{
	struct bp_matrix_state state;
	python_device_check(bp_matrix_get(&state));
	if (state.owner != BP_MATRIX_APPLICATION) {
		python_device_check(-EBUSY);
	}
	mp_hal_delay_ms(1);
}

static void write_pixels(matrix_obj_t *self, uint32_t first, const uint8_t *pixels, size_t count)
{
	int err;
	while ((err = bp_matrix_write(first, pixels, count)) == -EBUSY) {
		wait_available();
	}
	python_device_check(err);
	stop_text(self);
	wait_applied();
}

static void rows_pixels(const uint8_t rows[5], uint8_t pixels[BP_MATRIX_PIXELS])
{
	for (size_t row = 0; row < 5; row++) {
		for (size_t col = 0; col < 5; col++) {
			pixels[row * 5 + col] = rows[row] & (1U << (4 - col)) ? 100 : 0;
		}
	}
}

static void write_rows(matrix_obj_t *self, const uint8_t rows[5], bool negative)
{
	uint8_t pixels[BP_MATRIX_PIXELS];
	rows_pixels(rows, pixels);
	if (negative) {
		pixels[12] = 50;
	}
	write_pixels(self, 0, pixels, sizeof(pixels));
}

static mp_obj_t matrix_orientation(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, matrix_obj_t, self, PB_ARG_REQUIRED(up));
	(void)self;
	uint32_t rotation = 0;
	switch (pb_type_enum_get_value(up_in, &pb_enum_type_Side)) {
	case PBIO_GEOMETRY_SIDE_RIGHT: rotation = 1; break;
	case PBIO_GEOMETRY_SIDE_BACK:
	case PBIO_GEOMETRY_SIDE_BOTTOM: rotation = 2; break;
	case PBIO_GEOMETRY_SIDE_LEFT: rotation = 3; break;
	default: break;
	}
	python_device_check(bp_matrix_orientation(rotation));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_orientation_obj, 1, matrix_orientation);

static mp_obj_t matrix_char(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, matrix_obj_t, self, PB_ARG_REQUIRED(char));
	if (!mp_obj_is_qstr(char_in)) {
		pb_assert_type(char_in, &mp_type_str);
	}
	GET_STR_DATA_LEN(char_in, text, length);
	if (length != 1 || text[0] < 32 || text[0] > 126) {
		mp_raise_ValueError(MP_ERROR_TEXT("expected one printable ASCII character"));
	}
	write_rows(self, pb_font_5x5[text[0] - 32], false);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_char_obj, 1, matrix_char);

static mp_obj_t matrix_number(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, matrix_obj_t, self, PB_ARG_REQUIRED(number));
	mp_int_t number = pb_obj_get_int(number_in);
	if (number < -99 || number > 99) {
		write_rows(self, pb_font_5x5[(number < 0 ? '<' : '>') - 32], false);
	} else {
		bool negative = number < 0;
		if (negative) number = -number;
		uint8_t rows[5];
		for (size_t i = 0; i < 5; i++) {
			rows[i] = pb_digits_5x2[number / 10][i] << 3 | pb_digits_5x2[number % 10][i];
		}
		write_rows(self, rows, negative);
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_number_obj, 1, matrix_number);

static void icon_pixels(mp_obj_t icon_in, uint8_t pixels[BP_MATRIX_PIXELS])
{
	if (mp_obj_is_type(icon_in, &pb_type_Matrix)) {
		const pb_type_Matrix_obj_t *matrix = MP_OBJ_TO_PTR(icon_in);
		if (matrix->m != 5 || matrix->n != 5) {
			mp_raise_ValueError(MP_ERROR_TEXT("expected a 5x5 image"));
		}
		for (size_t row = 0; row < 5; row++) {
			for (size_t col = 0; col < 5; col++) {
				float value = pb_type_Matrix_get_scalar(icon_in, row, col);
				if (!isfinite(value)) {
					mp_raise_ValueError(MP_ERROR_TEXT("brightness must be finite"));
				}
				pixels[row * 5 + col] = value < 0 ? 0 : value > 100 ? 100 : value;
			}
		}
	} else {
		mp_obj_t *rows;
		size_t count;
		mp_obj_get_array(icon_in, &count, &rows);
		if (count != 5) mp_raise_ValueError(MP_ERROR_TEXT("expected five rows"));
		for (size_t row = 0; row < 5; row++) {
			mp_obj_t *columns;
			mp_obj_get_array(rows[row], &count, &columns);
			if (count != 5) mp_raise_ValueError(MP_ERROR_TEXT("expected five columns"));
			for (size_t col = 0; col < 5; col++) {
				pixels[row * 5 + col] = pb_obj_get_pct(columns[col]);
			}
		}
	}
}

static mp_obj_t matrix_icon(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, matrix_obj_t, self, PB_ARG_REQUIRED(icon));
	uint8_t pixels[BP_MATRIX_PIXELS];
	icon_pixels(icon_in, pixels);
	write_pixels(self, 0, pixels, sizeof(pixels));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_icon_obj, 1, matrix_icon);

static mp_obj_t matrix_on(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, matrix_obj_t, self, PB_ARG_DEFAULT_INT(brightness, 100));
	uint8_t pixels[BP_MATRIX_PIXELS];
	memset(pixels, pb_obj_get_pct(brightness_in), sizeof(pixels));
	write_pixels(self, 0, pixels, sizeof(pixels));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_on_obj, 1, matrix_on);

static mp_obj_t matrix_off(mp_obj_t self_in)
{
	matrix_obj_t *self = MP_OBJ_TO_PTR(self_in);
	const uint8_t pixels[BP_MATRIX_PIXELS] = {0};
	write_pixels(self, 0, pixels, sizeof(pixels));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(matrix_off_obj, matrix_off);

static mp_obj_t matrix_pixel(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, matrix_obj_t, self,
		PB_ARG_REQUIRED(row), PB_ARG_REQUIRED(column), PB_ARG_DEFAULT_INT(brightness, 100));
	mp_int_t row = pb_obj_get_int(row_in), col = pb_obj_get_int(column_in);
	uint8_t value = pb_obj_get_pct(brightness_in);
	if (row >= 0 && row < 5 && col >= 0 && col < 5) {
		write_pixels(self, row * 5 + col, &value, 1);
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_pixel_obj, 1, matrix_pixel);

static uint32_t duration(mp_obj_t value, bool zero_allowed)
{
	mp_int_t milliseconds = pb_obj_get_int(value);
	if (milliseconds < (zero_allowed ? 0 : 1) || milliseconds > INT32_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("invalid display duration"));
	}
	return milliseconds;
}

static mp_obj_t matrix_animate(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, matrix_obj_t, self,
		PB_ARG_REQUIRED(icons), PB_ARG_REQUIRED(interval));
	uint32_t milliseconds = duration(interval_in, false);
	mp_obj_t *icons;
	size_t count;
	mp_obj_get_array(icons_in, &count, &icons);
	if (count < 2 || count > BP_MATRIX_MAX_FRAMES) {
		mp_raise_ValueError(MP_ERROR_TEXT("expected 2..255 display frames"));
	}
	uint8_t *frames = m_new(uint8_t, count * BP_MATRIX_PIXELS);
	for (size_t i = 0; i < count; i++) {
		icon_pixels(icons[i], frames + i * BP_MATRIX_PIXELS);
	}
	int err;
	while ((err = bp_matrix_animate(frames, count, milliseconds)) == -EBUSY) {
		wait_available();
	}
	m_del(uint8_t, frames, count * BP_MATRIX_PIXELS);
	python_device_check(err);
	stop_text(self);
	wait_applied();
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_animate_obj, 1, matrix_animate);

static mp_obj_t text_close(mp_obj_t self_in)
{
	matrix_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (self->text != MP_OBJ_NULL) {
		stop_text(self);
		matrix_off(self_in);
	}
	return mp_const_none;
}

static pbio_error_t text_next(pbio_os_state_t *state, mp_obj_t self_in)
{
	matrix_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (self->text == MP_OBJ_NULL) {
		return PBIO_SUCCESS;
	}
	struct bp_matrix_state display;
	python_device_check(bp_matrix_get(&display));
	if (self->pending) {
		if (display.pending) {
			return PBIO_ERROR_AGAIN;
		}
		python_device_check(display.error);
		self->deadline = display.applied_ms + (self->blank ? self->off : self->on);
		self->pending = false;
	}
	if (bp_time_ms() < self->deadline) {
		return PBIO_ERROR_AGAIN;
	}
	GET_STR_DATA_LEN(self->text, text, length);
	if (self->blank && self->text_index == length) {
		self->text = MP_OBJ_NULL;
		return PBIO_SUCCESS;
	}
	bool blank = !self->blank && (self->off || self->text_index == length);
	uint8_t pixels[BP_MATRIX_PIXELS] = {0};
	if (!blank) {
		rows_pixels(pb_font_5x5[text[self->text_index] - 32], pixels);
	}
	int err = bp_matrix_write(0, pixels, sizeof(pixels));
	if (err == -EBUSY && display.owner == BP_MATRIX_APPLICATION) {
		return PBIO_ERROR_AGAIN;
	}
	python_device_check(err);
	self->blank = blank;
	self->text_index += !blank;
	self->pending = true;
	return PBIO_ERROR_AGAIN;
}

static mp_obj_t matrix_text(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, matrix_obj_t, self,
		PB_ARG_REQUIRED(text), PB_ARG_DEFAULT_INT(on, 500), PB_ARG_DEFAULT_INT(off, 50));
	uint32_t on = duration(on_in, true), off = duration(off_in, true);
	if (!mp_obj_is_qstr(text_in)) {
		pb_assert_type(text_in, &mp_type_str);
	}
	GET_STR_DATA_LEN(text_in, text, length);
	for (size_t i = 0; i < length; i++) {
		if (text[i] < 32 || text[i] > 126) {
			mp_raise_ValueError(MP_ERROR_TEXT("expected printable ASCII text"));
		}
	}
	if (!length) {
		return pb_type_async_return_result(mp_const_none, NULL);
	}
	/* Keep the string rooted while an awaitable displays it, including GC in
	 * another task. Each step uses the common nonblocking frame service. */
	stop_text(self);
	self->text = text_in;
	self->text_index = 0;
	self->on = on;
	self->off = off;
	self->blank = true;
	self->pending = false;
	self->deadline = 0;
	pb_type_async_t config = {
		.parent_obj = MP_OBJ_FROM_PTR(self),
		.iter_once = text_next,
		.close = text_close,
	};
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		mp_obj_t result = mp_const_none;
		if (pb_module_tools_run_loop_is_active()) {
			result = pb_type_async_wait_or_await(&config, &self->text_iter, true);
		} else {
			/* PBIO's indefinite event wait depends on its timer scheduler.
			 * Here the same state machine waits on the VM's own clock/events. */
			while (text_next(&config.state, config.parent_obj) == PBIO_ERROR_AGAIN) {
				int64_t remaining = self->deadline - bp_time_ms();
				mp_hal_delay_ms(self->pending || remaining <= 0 ? 1 : remaining);
			}
		}
		nlr_pop();
		return result;
	}
	/* Synchronous interruption also cancels the display. Cleanup must not
	 * replace the original error, including a controller failure. */
	nlr_buf_t cleanup;
	if (nlr_push(&cleanup) == 0) {
		text_close(MP_OBJ_FROM_PTR(self));
		nlr_pop();
	}
	nlr_jump(nlr.ret_val);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_text_obj, 1, matrix_text);

static const mp_rom_map_elem_t matrix_methods[] = {
	{ MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&matrix_on_obj) },
	{ MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&matrix_off_obj) },
	{ MP_ROM_QSTR(MP_QSTR_char), MP_ROM_PTR(&matrix_char_obj) },
	{ MP_ROM_QSTR(MP_QSTR_number), MP_ROM_PTR(&matrix_number_obj) },
	{ MP_ROM_QSTR(MP_QSTR_icon), MP_ROM_PTR(&matrix_icon_obj) },
	{ MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&matrix_pixel_obj) },
	{ MP_ROM_QSTR(MP_QSTR_orientation), MP_ROM_PTR(&matrix_orientation_obj) },
	{ MP_ROM_QSTR(MP_QSTR_animate), MP_ROM_PTR(&matrix_animate_obj) },
	{ MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&matrix_text_obj) },
};
static MP_DEFINE_CONST_DICT(matrix_dict, matrix_methods);
static MP_DEFINE_CONST_OBJ_TYPE(matrix_type, MP_QSTR_LightMatrix, MP_TYPE_FLAG_NONE, locals_dict, &matrix_dict);

mp_obj_t python_matrix_new(void)
{
	if (MP_STATE_PORT(python_hub_matrix) == MP_OBJ_NULL) {
		matrix_obj_t *self = mp_obj_malloc(matrix_obj_t, &matrix_type);
		*self = (matrix_obj_t){.base = {.type = &matrix_type}};
		MP_STATE_PORT(python_hub_matrix) = MP_OBJ_FROM_PTR(self);
	}
	matrix_obj_t *self = MP_OBJ_TO_PTR(MP_STATE_PORT(python_hub_matrix));
	int err = bp_matrix_orientation(0);
	if (err != -ENOTSUP) {
		python_device_check(err);
	}
	return MP_OBJ_FROM_PTR(self);
}

MP_REGISTER_ROOT_POINTER(mp_obj_t python_hub_matrix);
