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
#include <pybricks/tools/pb_type_matrix.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>
#include <pybricks/util_mp/pb_type_enum.h>

typedef struct {
	mp_obj_base_t base;
	pbio_geometry_side_t up;
} matrix_obj_t;

static size_t pixel_index(matrix_obj_t *self, size_t row, size_t col)
{
	switch (self->up) {
	case PBIO_GEOMETRY_SIDE_LEFT: return (4 - col) * 5 + row;
	case PBIO_GEOMETRY_SIDE_RIGHT: return col * 5 + 4 - row;
	case PBIO_GEOMETRY_SIDE_BACK:
	case PBIO_GEOMETRY_SIDE_BOTTOM: return (4 - row) * 5 + 4 - col;
	default: return row * 5 + col;
	}
}

static void write_pixels(uint32_t first, const uint8_t *pixels, size_t count)
{
	struct bp_matrix_state state;
	for (;;) {
		int err = bp_matrix_write(first, pixels, count);
		if (err != -EBUSY) {
			python_device_check(err);
			break;
		}
		python_device_check(bp_matrix_get(&state));
		if (state.owner != BP_MATRIX_APPLICATION) {
			python_device_check(err);
		}
		mp_hal_delay_ms(1);
	}
	do {
		python_device_check(bp_matrix_get(&state));
		if (state.pending) {
			mp_hal_delay_ms(1);
		}
	} while (state.pending);
	python_device_check(state.error);
}

static void write_rows(matrix_obj_t *self, const uint8_t rows[5], bool negative)
{
	uint8_t pixels[BP_MATRIX_PIXELS];
	for (size_t row = 0; row < 5; row++) {
		for (size_t col = 0; col < 5; col++) {
			pixels[pixel_index(self, row, col)] = rows[row] & (1U << (4 - col)) ? 100 : 0;
		}
	}
	if (negative) {
		pixels[12] = 50;
	}
	write_pixels(0, pixels, sizeof(pixels));
}

static mp_obj_t matrix_orientation(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, matrix_obj_t, self, PB_ARG_REQUIRED(up));
	self->up = pb_type_enum_get_value(up_in, &pb_enum_type_Side);
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

static mp_obj_t matrix_icon(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, matrix_obj_t, self, PB_ARG_REQUIRED(icon));
	uint8_t pixels[BP_MATRIX_PIXELS];
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
				pixels[pixel_index(self, row, col)] = value < 0 ? 0 : value > 100 ? 100 : value;
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
				pixels[pixel_index(self, row, col)] = pb_obj_get_pct(columns[col]);
			}
		}
	}
	write_pixels(0, pixels, sizeof(pixels));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_icon_obj, 1, matrix_icon);

static mp_obj_t matrix_on(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, matrix_obj_t, self, PB_ARG_DEFAULT_INT(brightness, 100));
	(void)self;
	uint8_t pixels[BP_MATRIX_PIXELS];
	memset(pixels, pb_obj_get_pct(brightness_in), sizeof(pixels));
	write_pixels(0, pixels, sizeof(pixels));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_on_obj, 1, matrix_on);

static mp_obj_t matrix_off(mp_obj_t self_in)
{
	const uint8_t pixels[BP_MATRIX_PIXELS] = {0};
	write_pixels(0, pixels, sizeof(pixels));
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
		write_pixels(pixel_index(self, row, col), &value, 1);
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrix_pixel_obj, 1, matrix_pixel);

static const mp_rom_map_elem_t matrix_methods[] = {
	{ MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&matrix_on_obj) },
	{ MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&matrix_off_obj) },
	{ MP_ROM_QSTR(MP_QSTR_char), MP_ROM_PTR(&matrix_char_obj) },
	{ MP_ROM_QSTR(MP_QSTR_number), MP_ROM_PTR(&matrix_number_obj) },
	{ MP_ROM_QSTR(MP_QSTR_icon), MP_ROM_PTR(&matrix_icon_obj) },
	{ MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&matrix_pixel_obj) },
	{ MP_ROM_QSTR(MP_QSTR_orientation), MP_ROM_PTR(&matrix_orientation_obj) },
};
static MP_DEFINE_CONST_DICT(matrix_dict, matrix_methods);
static MP_DEFINE_CONST_OBJ_TYPE(matrix_type, MP_QSTR_LightMatrix, MP_TYPE_FLAG_NONE, locals_dict, &matrix_dict);

mp_obj_t python_matrix_new(void)
{
	if (MP_STATE_PORT(python_hub_matrix) == MP_OBJ_NULL) {
		MP_STATE_PORT(python_hub_matrix) = MP_OBJ_FROM_PTR(mp_obj_malloc(matrix_obj_t, &matrix_type));
	}
	matrix_obj_t *self = MP_OBJ_TO_PTR(MP_STATE_PORT(python_hub_matrix));
	self->up = PBIO_GEOMETRY_SIDE_TOP;
	return MP_OBJ_FROM_PTR(self);
}

MP_REGISTER_ROOT_POINTER(mp_obj_t python_hub_matrix);
