// SPDX-License-Identifier: MIT
// Copyright (c) 2020-2023 The Pybricks Authors
// Adapted from pybricks/common/pb_type_imu.c; observations use the common SDK.
#include "devices.h"
#include <brickphyr.h>
#include <math.h>
#include <string.h>
#include <pbio/geometry.h>
#include <pybricks/parameters.h>
#include <pybricks/tools/pb_type_matrix.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_pb/pb_error.h>

static void observation(struct bp_imu_state *state)
{
	python_device_check(bp_imu_get_state(state));
	python_device_check(state->error);
}

static void measured_vector(bool calibrated, bool angular, pbio_geometry_xyz_t *vector)
{
	struct bp_imu_state state;
	struct bp_imu_sample raw;
	const int32_t *values;
	if (calibrated) {
		observation(&state);
		values = angular ? state.angular_velocity : state.acceleration;
	} else {
		python_device_check(bp_imu_get(&raw));
		python_device_check(raw.error);
		values = angular ? raw.angular_velocity : raw.acceleration;
	}
	for (size_t i = 0; i < 3; i++) {
		vector->values[i] = values[i] / 1000.0f;
	}
}

static mp_obj_t projected(mp_obj_t axis_in, pbio_geometry_xyz_t *vector)
{
	if (axis_in == mp_const_none) {
		return pb_type_Matrix_make_vector(3, vector->values, false);
	}
	if (!mp_obj_is_type(axis_in, &pb_type_Matrix)) {
		mp_raise_TypeError(MP_ERROR_TEXT("Axis must be Matrix."));
	}
	pb_type_Matrix_obj_t *matrix = MP_OBJ_TO_PTR(axis_in);
	if (matrix->m * matrix->n != 3) {
		mp_raise_ValueError(MP_ERROR_TEXT("Axis must be 1x3 or 3x1 matrix."));
	}
	pbio_geometry_xyz_t axis;
	for (size_t i = 0; i < 3; i++) {
		axis.values[i] = matrix->data[i] * matrix->scale;
	}
	float value;
	pb_assert(pbio_geometry_vector_project(&axis, vector, &value));
	return mp_obj_new_float_from_f(value);
}

static mp_obj_t imu_acceleration(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, mp_obj_base_t, self,
		PB_ARG_DEFAULT_NONE(axis), PB_ARG_DEFAULT_TRUE(calibrated));
	(void)self;
	pbio_geometry_xyz_t vector;
	measured_vector(mp_obj_is_true(calibrated_in), false, &vector);
	return projected(axis_in, &vector);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(imu_acceleration_obj, 1, imu_acceleration);

static mp_obj_t imu_angular_velocity(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, mp_obj_base_t, self,
		PB_ARG_DEFAULT_NONE(axis), PB_ARG_DEFAULT_TRUE(calibrated));
	(void)self;
	pbio_geometry_xyz_t vector;
	measured_vector(mp_obj_is_true(calibrated_in), true, &vector);
	return projected(axis_in, &vector);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(imu_angular_velocity_obj, 1, imu_angular_velocity);

static void up_vector(bool calibrated, pbio_geometry_xyz_t *vector)
{
	if (calibrated) {
		struct bp_imu_state state;
		observation(&state);
		memcpy(vector->values, state.orientation + 6, sizeof(vector->values));
	} else {
		measured_vector(false, false, vector);
	}
}

static mp_obj_t imu_up(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, mp_obj_base_t, self,
		PB_ARG_DEFAULT_TRUE(calibrated));
	(void)self;
	pbio_geometry_xyz_t vector;
	up_vector(mp_obj_is_true(calibrated_in), &vector);
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
	up_vector(mp_obj_is_true(calibrated_in), &vector);
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

static const mp_rom_map_elem_t imu_methods[] = {
	{ MP_ROM_QSTR(MP_QSTR_acceleration), MP_ROM_PTR(&imu_acceleration_obj) },
	{ MP_ROM_QSTR(MP_QSTR_angular_velocity), MP_ROM_PTR(&imu_angular_velocity_obj) },
	{ MP_ROM_QSTR(MP_QSTR_tilt), MP_ROM_PTR(&imu_tilt_obj) },
	{ MP_ROM_QSTR(MP_QSTR_up), MP_ROM_PTR(&imu_up_obj) },
	{ MP_ROM_QSTR(MP_QSTR_orientation), MP_ROM_PTR(&imu_orientation_obj) },
	{ MP_ROM_QSTR(MP_QSTR_ready), MP_ROM_PTR(&imu_ready_obj) },
	{ MP_ROM_QSTR(MP_QSTR_stationary), MP_ROM_PTR(&imu_stationary_obj) },
};
static MP_DEFINE_CONST_DICT(imu_dict, imu_methods);
static MP_DEFINE_CONST_OBJ_TYPE(imu_type, MP_QSTR_IMU, MP_TYPE_FLAG_NONE, locals_dict, &imu_dict);
const mp_obj_base_t python_imu = {&imu_type};
