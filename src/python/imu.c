// SPDX-License-Identifier: MIT
// Copyright (c) 2020-2023 The Pybricks Authors
// Adapted from pybricks/common/pb_type_imu.c; observations use the common SDK.
#include "devices.h"
#include "py/mphal.h"
#include <errno.h>
#include <brickphyr.h>
#include <math.h>
#include <string.h>
#include <pbio/geometry.h>
#include <pybricks/parameters.h>
#include <pybricks/tools/pb_type_matrix.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>
#include <pybricks/util_pb/pb_error.h>

static void observation(struct bp_imu_state *state)
{
	python_device_check(bp_imu_get_state(state));
	python_device_check(state->error);
}

static void map_vector(const float base[9], pbio_geometry_xyz_t *vector)
{
	pbio_geometry_matrix_3x3_t matrix;
	pbio_geometry_xyz_t mapped;
	memcpy(matrix.values, base, sizeof(matrix.values));
	pbio_geometry_vector_map(&matrix, vector, &mapped);
	*vector = mapped;
}

static void measured_vector(bool calibrated, bool angular, bool mapped, pbio_geometry_xyz_t *vector)
{
	struct bp_imu_application_state state;
	python_device_check(bp_imu_get_application(&state));
	struct bp_imu_sample raw;
	const int32_t *values;
	if (calibrated) {
		python_device_check(state.physical.error);
		values = angular ? state.physical.angular_velocity : state.physical.acceleration;
	} else {
		python_device_check(bp_imu_get(&raw));
		python_device_check(raw.error);
		values = angular ? raw.angular_velocity : raw.acceleration;
	}
	for (size_t i = 0; i < 3; i++) vector->values[i] = values[i] / 1000.0f;
	if (mapped) map_vector(state.base, vector);
}

static void extract_axis(mp_obj_t axis_in, pbio_geometry_xyz_t *axis)
{
	if (!mp_obj_is_type(axis_in, &pb_type_Matrix)) {
		mp_raise_TypeError(MP_ERROR_TEXT("Axis must be Matrix."));
	}
	pb_type_Matrix_obj_t *matrix = MP_OBJ_TO_PTR(axis_in);
	if (matrix->m * matrix->n != 3) {
		mp_raise_ValueError(MP_ERROR_TEXT("Axis must be 1x3 or 3x1 matrix."));
	}
	for (size_t i = 0; i < 3; i++) axis->values[i] = matrix->data[i] * matrix->scale;
}

static mp_obj_t projected(mp_obj_t axis_in, pbio_geometry_xyz_t *vector)
{
	if (axis_in == mp_const_none) return pb_type_Matrix_make_vector(3, vector->values, false);
	pbio_geometry_xyz_t axis;
	extract_axis(axis_in, &axis);
	float value;
	pb_assert(pbio_geometry_vector_project(&axis, vector, &value));
	return mp_obj_new_float_from_f(value);
}

static void command_check(int err)
{
	if (err == -EINVAL) mp_raise_ValueError(MP_ERROR_TEXT("Invalid IMU configuration."));
	python_device_check(err);
}

static void configure(const struct bp_imu_command *command)
{
	int err;
	while ((err = bp_imu_configure(command)) == -EBUSY) mp_hal_delay_ms(1);
	command_check(err);
	struct bp_imu_application_state state;
	do {
		python_device_check(bp_imu_get_application(&state));
		if (state.pending) mp_hal_delay_ms(1);
	} while (state.pending);
	command_check(state.command_error);
}

void python_imu_axes(mp_obj_t top, mp_obj_t front)
{
	pbio_geometry_xyz_t front_axis, top_axis;
	extract_axis(front, &front_axis);
	extract_axis(top, &top_axis);
	struct bp_imu_command command = {.flags = BP_IMU_FRAME_AXES};
	memcpy(command.front, front_axis.values, sizeof(command.front));
	memcpy(command.top, top_axis.values, sizeof(command.top));
	configure(&command);
}

static mp_obj_t imu_acceleration(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, mp_obj_base_t, self,
		PB_ARG_DEFAULT_NONE(axis), PB_ARG_DEFAULT_TRUE(calibrated));
	(void)self;
	pbio_geometry_xyz_t vector;
	measured_vector(mp_obj_is_true(calibrated_in), false, true, &vector);
	return projected(axis_in, &vector);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(imu_acceleration_obj, 1, imu_acceleration);

static mp_obj_t imu_angular_velocity(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, mp_obj_base_t, self,
		PB_ARG_DEFAULT_NONE(axis), PB_ARG_DEFAULT_TRUE(calibrated));
	(void)self;
	pbio_geometry_xyz_t vector;
	measured_vector(mp_obj_is_true(calibrated_in), true, true, &vector);
	return projected(axis_in, &vector);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(imu_angular_velocity_obj, 1, imu_angular_velocity);

static void up_vector(bool calibrated, bool mapped, pbio_geometry_xyz_t *vector)
{
	if (calibrated) {
		struct bp_imu_application_state state;
		python_device_check(bp_imu_get_application(&state));
		python_device_check(state.physical.error);
		memcpy(vector->values, state.physical.orientation + 6, sizeof(vector->values));
		if (mapped) map_vector(state.base, vector);
	} else {
		measured_vector(false, false, mapped, vector);
	}
}

static mp_obj_t imu_up(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, mp_obj_base_t, self,
		PB_ARG_DEFAULT_TRUE(calibrated));
	(void)self;
	pbio_geometry_xyz_t vector;
	up_vector(mp_obj_is_true(calibrated_in), false, &vector);
	switch (pbio_geometry_side_from_vector(&vector)) {
	case PBIO_GEOMETRY_SIDE_FRONT: return MP_OBJ_FROM_PTR(&pb_Side_FRONT_obj);
	case PBIO_GEOMETRY_SIDE_LEFT: return MP_OBJ_FROM_PTR(&pb_Side_LEFT_obj);
	case PBIO_GEOMETRY_SIDE_TOP: return MP_OBJ_FROM_PTR(&pb_Side_TOP_obj);
	case PBIO_GEOMETRY_SIDE_BACK: return MP_OBJ_FROM_PTR(&pb_Side_BACK_obj);
	case PBIO_GEOMETRY_SIDE_RIGHT: return MP_OBJ_FROM_PTR(&pb_Side_RIGHT_obj);
	default: return MP_OBJ_FROM_PTR(&pb_Side_BOTTOM_obj);
	}
}
static MP_DEFINE_CONST_FUN_OBJ_KW(imu_up_obj, 1, imu_up);

static mp_obj_t imu_tilt(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, mp_obj_base_t, self,
		PB_ARG_DEFAULT_TRUE(calibrated));
	(void)self;
	pbio_geometry_xyz_t vector;
	up_vector(mp_obj_is_true(calibrated_in), true, &vector);
	float pitch = atan2f(-vector.x, sqrtf(vector.z * vector.z + vector.y * vector.y));
	float roll = atan2f(vector.y, vector.z);
	mp_obj_t result[] = {mp_obj_new_float_from_f(pitch * 57.296f),
		mp_obj_new_float_from_f(roll * 57.296f)};
	return mp_obj_new_tuple(MP_ARRAY_SIZE(result), result);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(imu_tilt_obj, 1, imu_tilt);

static mp_obj_t imu_orientation(mp_obj_t self_in)
{
	struct bp_imu_state state;
	observation(&state);
	pb_type_Matrix_obj_t *matrix = MP_OBJ_TO_PTR(pb_type_Matrix_make_bitmap(3, 3, 1, 0));
	memcpy(matrix->data, state.orientation, sizeof(state.orientation));
	return MP_OBJ_FROM_PTR(matrix);
}
static MP_DEFINE_CONST_FUN_OBJ_1(imu_orientation_obj, imu_orientation);

static mp_obj_t imu_ready(mp_obj_t self_in)
{
	struct bp_imu_state state;
	python_device_check(bp_imu_get_state(&state));
	return mp_obj_new_bool(!state.error && state.ready);
}
static MP_DEFINE_CONST_FUN_OBJ_1(imu_ready_obj, imu_ready);

static mp_obj_t imu_stationary(mp_obj_t self_in)
{
	struct bp_imu_state state;
	python_device_check(bp_imu_get_state(&state));
	return mp_obj_new_bool(!state.error && state.stationary);
}
static MP_DEFINE_CONST_FUN_OBJ_1(imu_stationary_obj, imu_stationary);

static mp_obj_t imu_heading(mp_obj_t self_in)
{
	struct bp_imu_application_state state;
	python_device_check(bp_imu_get_application(&state));
	python_device_check(state.physical.error);
	return mp_obj_new_float_from_f(state.heading_3d);
}
static MP_DEFINE_CONST_FUN_OBJ_1(imu_heading_obj, imu_heading);

/* Brickphyr extension: integrations on opposite sides of a data gap cannot be
 * compared. The same reference identifier is exposed by the native snapshot. */
static mp_obj_t imu_epoch(mp_obj_t self_in)
{
	struct bp_imu_state state;
	observation(&state);
	return mp_obj_new_int_from_uint(state.epoch);
}
static MP_DEFINE_CONST_FUN_OBJ_1(imu_epoch_obj, imu_epoch);

static mp_obj_t imu_reset_heading(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, mp_obj_base_t, self, PB_ARG_REQUIRED(angle));
	(void)self;
	struct bp_imu_command command = {.flags = BP_IMU_FRAME_HEADING, .heading = mp_obj_get_float(angle_in)};
	configure(&command);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(imu_reset_heading_obj, 1, imu_reset_heading);

static mp_obj_t imu_rotation(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, mp_obj_base_t, self,
		PB_ARG_DEFAULT_NONE(axis), PB_ARG_DEFAULT_TRUE(calibrated));
	(void)self;
	pbio_geometry_xyz_t axis, vector;
	extract_axis(axis_in, &axis);
	struct bp_imu_application_state state;
	python_device_check(bp_imu_get_application(&state));
	python_device_check(state.physical.error);
	memcpy(vector.values, mp_obj_is_true(calibrated_in) ? state.physical.rotation :
		state.rotation_uncalibrated, sizeof(vector.values));
	map_vector(state.base, &vector);
	float angle;
	pb_assert(pbio_geometry_vector_project(&axis, &vector, &angle));
	return mp_obj_new_float_from_f(angle);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(imu_rotation_obj, 1, imu_rotation);

static int32_t setting_value(mp_obj_t value)
{
	float scaled = mp_obj_get_float(value) * 1000.0f;
	if (!isfinite(scaled) || scaled < -2147483648.0f || scaled >= 2147483648.0f) {
		mp_raise_ValueError(MP_ERROR_TEXT("IMU setting out of range."));
	}
	return lroundf(scaled);
}

static mp_obj_t imu_settings(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, mp_obj_base_t, self,
		PB_ARG_DEFAULT_NONE(angular_velocity_threshold), PB_ARG_DEFAULT_NONE(acceleration_threshold),
		PB_ARG_DEFAULT_NONE(heading_correction), PB_ARG_DEFAULT_NONE(angular_velocity_bias),
		PB_ARG_DEFAULT_NONE(angular_velocity_scale), PB_ARG_DEFAULT_NONE(acceleration_correction));
	(void)self;
	const size_t counts[] = {1, 1, 3, 3, 6, 1};
	/* Python interleaves gravity's positive/negative calibration per axis. */
	const size_t gravity_order[] = {0, 3, 1, 4, 2, 5};
	if (PB_PARSE_ARGS_METHOD_ALL_NONE()) {
		struct bp_imu_application_state state;
		python_device_check(bp_imu_get_application(&state));
		mp_obj_t result[BP_IMU_SETTING_COUNT];
		for (size_t group = 0; group < MP_ARRAY_SIZE(result); group++) {
			mp_obj_t values[6];
			for (size_t i = 0; i < counts[group]; i++) {
				size_t index = group == BP_IMU_ACCEL_CALIBRATION ? gravity_order[i] : i;
				values[i] = mp_obj_new_float_from_f(state.settings.values[group][index] / 1000.0f);
			}
			result[group] = counts[group] == 1 ? values[0] : mp_obj_new_tuple(counts[group], values);
		}
		return mp_obj_new_tuple(MP_ARRAY_SIZE(result), result);
	}
	mp_obj_t supplied[] = {angular_velocity_threshold_in, acceleration_threshold_in,
		angular_velocity_bias_in, angular_velocity_scale_in, acceleration_correction_in, heading_correction_in};
	struct bp_imu_command command = {.flags = BP_IMU_CALIBRATION};
	for (size_t group = 0; group < MP_ARRAY_SIZE(supplied); group++) {
		if (supplied[group] == mp_const_none) continue;
		mp_obj_t *values = &supplied[group];
		size_t count = 1;
		if (counts[group] != 1) mp_obj_get_array(supplied[group], &count, &values);
		if (count != counts[group]) mp_raise_ValueError(MP_ERROR_TEXT("Invalid IMU tuple length."));
		for (size_t i = 0; i < count; i++) {
			size_t index = group == BP_IMU_ACCEL_CALIBRATION ? gravity_order[i] : i;
			command.settings.values[group][index] = setting_value(values[i]);
		}
		command.settings_mask |= 1U << group;
	}
	configure(&command);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(imu_settings_obj, 1, imu_settings);

static const mp_rom_map_elem_t imu_methods[] = {
	{ MP_ROM_QSTR(MP_QSTR_acceleration), MP_ROM_PTR(&imu_acceleration_obj) },
	{ MP_ROM_QSTR(MP_QSTR_angular_velocity), MP_ROM_PTR(&imu_angular_velocity_obj) },
	{ MP_ROM_QSTR(MP_QSTR_tilt), MP_ROM_PTR(&imu_tilt_obj) },
	{ MP_ROM_QSTR(MP_QSTR_up), MP_ROM_PTR(&imu_up_obj) },
	{ MP_ROM_QSTR(MP_QSTR_orientation), MP_ROM_PTR(&imu_orientation_obj) },
	{ MP_ROM_QSTR(MP_QSTR_ready), MP_ROM_PTR(&imu_ready_obj) },
	{ MP_ROM_QSTR(MP_QSTR_stationary), MP_ROM_PTR(&imu_stationary_obj) },
	{ MP_ROM_QSTR(MP_QSTR_heading), MP_ROM_PTR(&imu_heading_obj) },
	{ MP_ROM_QSTR(MP_QSTR_reset_heading), MP_ROM_PTR(&imu_reset_heading_obj) },
	{ MP_ROM_QSTR(MP_QSTR_rotation), MP_ROM_PTR(&imu_rotation_obj) },
	{ MP_ROM_QSTR(MP_QSTR_settings), MP_ROM_PTR(&imu_settings_obj) },
	{ MP_ROM_QSTR(MP_QSTR_epoch), MP_ROM_PTR(&imu_epoch_obj) },
};
static MP_DEFINE_CONST_DICT(imu_dict, imu_methods);
static MP_DEFINE_CONST_OBJ_TYPE(imu_type, MP_QSTR_IMU, MP_TYPE_FLAG_NONE, locals_dict, &imu_dict);
const mp_obj_base_t python_imu = {&imu_type};
