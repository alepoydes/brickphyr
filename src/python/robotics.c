// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
/* Adapted from robotics/pb_type_drivebase.c at a7b1b04ae. The shared native
 * service owns both motor controllers; Python retains handles and awaitables. */
#include "devices.h"
#include "py/mphal.h"
#include <brickphyr.h>
#include <errno.h>
#include <math.h>
#include <pybricks/parameters.h>
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>
#include <pybricks/util_mp/pb_type_enum.h>

typedef struct {
	mp_obj_base_t base;
	mp_obj_t motors[2];
	uint32_t handle, motion_request;
	pb_type_async_t *iter;
} drivebase_obj_t;

static void check(int err)
{
	if (err == -EINVAL || err == -ERANGE) mp_raise_ValueError(MP_ERROR_TEXT("invalid drivebase setting"));
	python_device_check(err);
}

static struct bp_drivebase_state snapshot(drivebase_obj_t *self)
{
	struct bp_drivebase_state value;
	check(bp_drivebase_get(self->handle, &value));
	check(value.error);
	check(value.cleanup_error);
	return value;
}

static void applied(drivebase_obj_t *self, uint32_t request)
{
	for (;;) {
		struct bp_drivebase_state value = snapshot(self);
		if (value.request != request) check(-ECANCELED);
		if (!value.pending && value.completed == request) {
			check(value.command_error);
			return;
		}
		mp_hal_delay_ms(2);
	}
}

static mp_obj_t new_pair(const mp_obj_type_t *type, mp_obj_t left, mp_obj_t right,
	int32_t wheel, int32_t track, bool spike)
{
	struct bp_drivebase_setup setup = {.wheel_um = wheel, .track_um = track, .spike = spike};
	python_motor_connection(left, &setup.ports[0], &setup.generations[0]);
	python_motor_connection(right, &setup.ports[1], &setup.generations[1]);
	drivebase_obj_t *self = mp_obj_malloc(drivebase_obj_t, type);
	*self = (drivebase_obj_t){.base = {.type = type}, .motors = {left, right}};
	for (;;) {
		int err = bp_drivebase_open(&setup, &self->handle);
		if (err != -EAGAIN) { check(err); break; }
		mp_hal_delay_ms(2);
	}
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		applied(self, 1);
		nlr_pop();
		return MP_OBJ_FROM_PTR(self);
	}
	/* A caught constructor failure must not leave its pair attached to the
	 * independently owned Motor objects. Preserve the initialization error. */
	bp_drivebase_close(self->handle);
	nlr_jump(nlr.ret_val);
}

static mp_obj_t drivebase_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(left_motor), PB_ARG_REQUIRED(right_motor),
		PB_ARG_REQUIRED(wheel_diameter), PB_ARG_REQUIRED(axle_track));
	return new_pair(type, left_motor_in, right_motor_in,
		pb_obj_get_scaled_int(wheel_diameter_in, 1000), pb_obj_get_scaled_int(axle_track_in, 1000), false);
}

static bool owns_motion(const struct bp_drivebase_state *value, uint32_t request)
{
	return (value->pending && value->request == request) ||
		(value->motion == request && (value->request != request || !value->command_error));
}

static mp_obj_t cancel_motion(mp_obj_t self_in)
{
	drivebase_obj_t *self = MP_OBJ_TO_PTR(self_in);
	struct bp_drivebase_state value;
	int err = bp_drivebase_get(self->handle, &value);
	if (err == -ENODEV) return mp_const_none;
	check(err);
	/* A rejected or individually cancelled pair command does not own any
	 * newer Motor motion. Closing that old awaitable must leave it alone. */
	if (owns_motion(&value, self->motion_request) && (value.active || value.pending)) {
		struct bp_drivebase_command command = {.action = BP_DRIVEBASE_COAST};
		uint32_t request;
		check(bp_drivebase_command(self->handle, &command, &request));
		applied(self, request);
	}
	return mp_const_none;
}

static void motion_error(drivebase_obj_t *self, void *error)
{
	nlr_buf_t cleanup;
	if (nlr_push(&cleanup) == 0) {
		cancel_motion(MP_OBJ_FROM_PTR(self));
		nlr_pop();
	}
	nlr_jump(error);
}

static pbio_error_t motion_next(pbio_os_state_t *state, mp_obj_t self_in)
{
	drivebase_obj_t *self = MP_OBJ_TO_PTR(self_in);
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		struct bp_drivebase_state value = snapshot(self);
		bool done = value.motion != self->motion_request;
		if (!done) {
			if (value.request == self->motion_request && value.command_error != -ECANCELED) check(value.command_error);
			done = !value.pending && value.done;
		}
		nlr_pop();
		return done ? PBIO_SUCCESS : PBIO_ERROR_AGAIN;
	}
	motion_error(self, nlr.ret_val);
	return PBIO_SUCCESS;
}

static mp_obj_t move(drivebase_obj_t *self, struct bp_drivebase_command command, bool wait)
{
	uint32_t request;
	check(bp_drivebase_command(self->handle, &command, &request));
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		applied(self, request);
		self->motion_request = request;
		pb_type_async_schedule_stop_iteration(self->iter);
		mp_obj_t result = mp_const_none;
		if (wait) {
			pb_type_async_t config = {.parent_obj = MP_OBJ_FROM_PTR(self), .iter_once = motion_next, .close = cancel_motion};
			if (pb_module_tools_run_loop_is_active()) result = pb_type_async_wait_or_await(&config, &self->iter, true);
			else while (motion_next(NULL, MP_OBJ_FROM_PTR(self)) == PBIO_ERROR_AGAIN) mp_hal_delay_ms(2);
		}
		nlr_pop();
		return result;
	}
	/* A rejected replacement leaves the previous maneuver and its awaitable
	 * intact. An interrupted accepted request still needs bounded cleanup. */
	struct bp_drivebase_state value;
	if (!bp_drivebase_get(self->handle, &value) && owns_motion(&value, request)) {
		self->motion_request = request;
		pb_type_async_schedule_stop_iteration(self->iter);
		motion_error(self, nlr.ret_val);
	}
	nlr_jump(nlr.ret_val);
	return mp_const_none;
}

static mp_obj_t straight(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, drivebase_obj_t, self, PB_ARG_REQUIRED(distance),
		PB_ARG_DEFAULT_OBJ(then, pb_Stop_HOLD_obj), PB_ARG_DEFAULT_TRUE(wait));
	return move(self, (struct bp_drivebase_command){.action = BP_DRIVEBASE_RELATIVE,
		.distance = pb_obj_get_int(distance_in), .finish = pb_type_enum_get_value(then_in, &pb_enum_type_Stop)},
		mp_obj_is_true(wait_in));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(straight_obj, 1, straight);

static mp_obj_t turn(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, drivebase_obj_t, self, PB_ARG_REQUIRED(angle),
		PB_ARG_DEFAULT_OBJ(then, pb_Stop_HOLD_obj), PB_ARG_DEFAULT_TRUE(wait), PB_ARG_DEFAULT_FALSE(absolute));
	return move(self, (struct bp_drivebase_command){.action = mp_obj_is_true(absolute_in) ? BP_DRIVEBASE_TURN : BP_DRIVEBASE_RELATIVE,
		.angle = pb_obj_get_int(angle_in), .finish = pb_type_enum_get_value(then_in, &pb_enum_type_Stop)}, mp_obj_is_true(wait_in));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(turn_obj, 1, turn);

static mp_obj_t arc(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, drivebase_obj_t, self, PB_ARG_REQUIRED(radius),
		PB_ARG_DEFAULT_NONE(angle), PB_ARG_DEFAULT_NONE(distance), PB_ARG_DEFAULT_OBJ(then, pb_Stop_HOLD_obj), PB_ARG_DEFAULT_TRUE(wait));
	bool by_distance = distance_in != mp_const_none;
	if (by_distance == (angle_in != mp_const_none)) mp_raise_ValueError(MP_ERROR_TEXT("specify distance or angle"));
	struct bp_drivebase_command command = {.action = by_distance ? BP_DRIVEBASE_ARC_DISTANCE : BP_DRIVEBASE_ARC_ANGLE,
		.radius = pb_obj_get_int(radius_in), .finish = pb_type_enum_get_value(then_in, &pb_enum_type_Stop)};
	if (by_distance) command.distance = pb_obj_get_int(distance_in);
	else command.angle = pb_obj_get_int(angle_in);
	return move(self, command, mp_obj_is_true(wait_in));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(arc_obj, 1, arc);

static mp_obj_t curve(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, drivebase_obj_t, self, PB_ARG_REQUIRED(radius), PB_ARG_REQUIRED(angle),
		PB_ARG_DEFAULT_OBJ(then, pb_Stop_HOLD_obj), PB_ARG_DEFAULT_TRUE(wait));
	return move(self, (struct bp_drivebase_command){.action = BP_DRIVEBASE_CURVE,
		.radius = pb_obj_get_int(radius_in), .angle = pb_obj_get_int(angle_in),
		.finish = pb_type_enum_get_value(then_in, &pb_enum_type_Stop)}, mp_obj_is_true(wait_in));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(curve_obj, 1, curve);

static mp_obj_t drive(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, drivebase_obj_t, self, PB_ARG_REQUIRED(speed), PB_ARG_REQUIRED(turn_rate));
	return move(self, (struct bp_drivebase_command){.action = BP_DRIVEBASE_TIMED,
		.speed = pb_obj_get_int(speed_in), .turn_rate = pb_obj_get_int(turn_rate_in),
		.duration_ms = 300000, .finish = BP_MOTOR_FINISH_CONTINUE}, false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(drive_obj, 1, drive);

static mp_obj_t stop(mp_obj_t self) { return move(MP_OBJ_TO_PTR(self), (struct bp_drivebase_command){.action = BP_DRIVEBASE_COAST}, false); }
static MP_DEFINE_CONST_FUN_OBJ_1(stop_obj, stop);
static mp_obj_t brake(mp_obj_t self) { return move(MP_OBJ_TO_PTR(self), (struct bp_drivebase_command){.action = BP_DRIVEBASE_BRAKE}, false); }
static MP_DEFINE_CONST_FUN_OBJ_1(brake_obj, brake);
static mp_obj_t hold(mp_obj_t self) { return move(MP_OBJ_TO_PTR(self), (struct bp_drivebase_command){.action = BP_DRIVEBASE_RELATIVE, .finish = BP_MOTOR_FINISH_HOLD}, false); }
static MP_DEFINE_CONST_FUN_OBJ_1(hold_obj, hold);

static mp_obj_t distance(mp_obj_t self) { return mp_obj_new_int(snapshot(MP_OBJ_TO_PTR(self)).distance); }
static MP_DEFINE_CONST_FUN_OBJ_1(distance_obj, distance);
static mp_obj_t angle(mp_obj_t self) { return mp_obj_new_float_from_f(snapshot(MP_OBJ_TO_PTR(self)).angle); }
static MP_DEFINE_CONST_FUN_OBJ_1(angle_obj, angle);
static mp_obj_t done(mp_obj_t self)
{
	struct bp_drivebase_state value = snapshot(MP_OBJ_TO_PTR(self));
	return mp_obj_new_bool(!value.pending && value.done);
}
static MP_DEFINE_CONST_FUN_OBJ_1(done_obj, done);
static mp_obj_t stalled(mp_obj_t self) { return mp_obj_new_bool(snapshot(MP_OBJ_TO_PTR(self)).stalled); }
static MP_DEFINE_CONST_FUN_OBJ_1(stalled_obj, stalled);

static mp_obj_t state(mp_obj_t self)
{
	struct bp_drivebase_state value = snapshot(MP_OBJ_TO_PTR(self));
	mp_obj_t values[] = {mp_obj_new_int(value.distance), mp_obj_new_int(value.speed),
		mp_obj_new_int(value.heading), mp_obj_new_int(value.turn_rate)};
	return mp_obj_new_tuple(MP_ARRAY_SIZE(values), values);
}
static MP_DEFINE_CONST_FUN_OBJ_1(state_obj, state);

static mp_obj_t reset(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, drivebase_obj_t, self, PB_ARG_DEFAULT_INT(distance, 0), PB_ARG_DEFAULT_INT(angle, 0));
	return move(self, (struct bp_drivebase_command){.action = BP_DRIVEBASE_RESET,
		.distance = pb_obj_get_int(distance_in), .angle = pb_obj_get_int(angle_in)}, false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(reset_obj, 1, reset);

static mp_obj_t acceleration_value(int32_t a, int32_t d)
{
	mp_obj_t values[] = {mp_obj_new_int(a), mp_obj_new_int(d)};
	return a == d ? values[0] : mp_obj_new_tuple(2, values);
}

static void acceleration_set(mp_obj_t input, int32_t *a, int32_t *d)
{
	if (input == mp_const_none) return;
	if (mp_obj_is_type(input, &mp_type_tuple) || mp_obj_is_type(input, &mp_type_list)) {
		mp_obj_t *values;
		mp_obj_get_array_fixed_n(input, 2, &values);
		*a = pb_obj_get_default_abs_int(values[0], 0);
		*d = pb_obj_get_default_abs_int(values[1], 0);
	} else *a = *d = pb_obj_get_default_abs_int(input, 0);
}

static mp_obj_t settings(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, drivebase_obj_t, self,
		PB_ARG_DEFAULT_NONE(straight_speed), PB_ARG_DEFAULT_NONE(straight_acceleration),
		PB_ARG_DEFAULT_NONE(turn_rate), PB_ARG_DEFAULT_NONE(turn_acceleration));
	struct bp_drivebase_settings s = snapshot(self).settings;
	if (PB_PARSE_ARGS_METHOD_ALL_NONE()) {
		mp_obj_t values[] = {mp_obj_new_int(s.speed), acceleration_value(s.acceleration, s.deceleration),
			mp_obj_new_int(s.turn_rate), acceleration_value(s.turn_acceleration, s.turn_deceleration)};
		return mp_obj_new_tuple(MP_ARRAY_SIZE(values), values);
	}
	s.speed = pb_obj_get_default_abs_int(straight_speed_in, s.speed);
	s.turn_rate = pb_obj_get_default_abs_int(turn_rate_in, s.turn_rate);
	acceleration_set(straight_acceleration_in, &s.acceleration, &s.deceleration);
	acceleration_set(turn_acceleration_in, &s.turn_acceleration, &s.turn_deceleration);
	uint32_t request;
	check(bp_drivebase_settings_set(self->handle, &s, &request));
	applied(self, request);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(settings_obj, 1, settings);

static mp_obj_t use_gyro(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, drivebase_obj_t, self, PB_ARG_REQUIRED(use_gyro));
	struct bp_drivebase_command command = {.action = BP_DRIVEBASE_GYRO, .gyro = mp_obj_is_true(use_gyro_in)};
	uint32_t request;
	check(bp_drivebase_command(self->handle, &command, &request));
	applied(self, request);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(use_gyro_obj, 1, use_gyro);

static const mp_rom_map_elem_t drivebase_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_straight), MP_ROM_PTR(&straight_obj)},
	{MP_ROM_QSTR(MP_QSTR_turn), MP_ROM_PTR(&turn_obj)},
	{MP_ROM_QSTR(MP_QSTR_arc), MP_ROM_PTR(&arc_obj)},
	{MP_ROM_QSTR(MP_QSTR_curve), MP_ROM_PTR(&curve_obj)},
	{MP_ROM_QSTR(MP_QSTR_drive), MP_ROM_PTR(&drive_obj)},
	{MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_brake), MP_ROM_PTR(&brake_obj)},
	{MP_ROM_QSTR(MP_QSTR_hold), MP_ROM_PTR(&hold_obj)},
	{MP_ROM_QSTR(MP_QSTR_distance), MP_ROM_PTR(&distance_obj)},
	{MP_ROM_QSTR(MP_QSTR_angle), MP_ROM_PTR(&angle_obj)},
	{MP_ROM_QSTR(MP_QSTR_state), MP_ROM_PTR(&state_obj)},
	{MP_ROM_QSTR(MP_QSTR_done), MP_ROM_PTR(&done_obj)},
	{MP_ROM_QSTR(MP_QSTR_stalled), MP_ROM_PTR(&stalled_obj)},
	{MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&reset_obj)},
	{MP_ROM_QSTR(MP_QSTR_settings), MP_ROM_PTR(&settings_obj)},
	{MP_ROM_QSTR(MP_QSTR_use_gyro), MP_ROM_PTR(&use_gyro_obj)},
};
static MP_DEFINE_CONST_DICT(drivebase_dict, drivebase_methods);
static MP_DEFINE_CONST_OBJ_TYPE(drivebase_type, MP_QSTR_DriveBase, MP_TYPE_FLAG_NONE,
	make_new, drivebase_new, locals_dict, &drivebase_dict);
static const mp_rom_map_elem_t robotics_globals[] = {
	{MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_robotics)},
	{MP_ROM_QSTR(MP_QSTR_DriveBase), MP_ROM_PTR(&drivebase_type)},
};
static MP_DEFINE_CONST_DICT(robotics_dict, robotics_globals);
const mp_obj_module_t pb_module_robotics = {{&mp_type_module}, (mp_obj_dict_t *)&robotics_dict};
