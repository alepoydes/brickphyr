// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
/* Adapted from pybricks/common/pb_type_motor.c at a7b1b04ae.
 * Direction, voltage limits and output ownership live in the native service. */
#include "devices.h"
#include "py/mphal.h"
#include <brickphyr.h>
#include <errno.h>
#include <zephyr/sys/util.h>
#include <pbio/dcmotor.h>
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/parameters.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>
#include <pybricks/util_mp/pb_type_enum.h>

typedef struct {
	mp_obj_base_t base;
	uint32_t port, generation;
	bool closed;
	uint32_t motion_request;
	pb_type_async_t *iter;
} motor_obj_t;

static void check(int err)
{
	if (err == -EINVAL) mp_raise_ValueError(MP_ERROR_TEXT("invalid motor setting"));
	python_device_check(err == -ESTALE ? -ENODEV : err);
}

static void snapshot(motor_obj_t *self, struct bp_motor_state *value)
{
	if (self->closed) check(-ENODEV);
	check(bp_motor_get(self->port, value));
	if (value->generation != self->generation) check(-ENODEV);
}

static void configured(motor_obj_t *self, struct bp_motor_state *value)
{
	snapshot(self, value);
	check(value->error);
	if (value->owner != BP_PORT_APPLICATION || !value->direction) check(-ENODEV);
}

static void applied(motor_obj_t *self, uint32_t request)
{
	for (;;) {
		struct bp_motor_state value;
		snapshot(self, &value);
		if (value.request != request) check(-ECANCELED);
		if (value.completed == request && !value.pending) {
			check(value.error);
			check(value.cleanup_error);
			if (!value.valid) check(-EIO);
			return;
		}
		mp_hal_delay_ms(2);
	}
}

/* Parse single or compounded gear trains using Python integers so valid ratios
 * do not overflow intermediate products. Interior gears do not change ratio. */
static int32_t gear_ratio(mp_obj_t gears)
{
	if (gears == mp_const_none) return 1000;
	size_t count;
	mp_obj_t *items;
	mp_obj_get_array(gears, &count, &items);
	if (!count) check(-EINVAL);
	bool single = mp_obj_is_int(items[0]);
	mp_obj_t numerator = MP_OBJ_NEW_SMALL_INT(1000), denominator = MP_OBJ_NEW_SMALL_INT(1);
	for (size_t i = 0; i < (single ? 1 : count); i++) {
		size_t length = count;
		mp_obj_t *train = items;
		if (!single) mp_obj_get_array(items[i], &length, &train);
		if (!length) check(-EINVAL);
		for (size_t j = 0; j < length; j++) {
			if (!mp_obj_is_int(train[j]) || mp_obj_get_int(train[j]) < 1) check(-EINVAL);
		}
		numerator = mp_binary_op(MP_BINARY_OP_MULTIPLY, numerator, train[length - 1]);
		denominator = mp_binary_op(MP_BINARY_OP_MULTIPLY, denominator, train[0]);
	}
	int32_t ratio = mp_obj_get_int(mp_binary_op(MP_BINARY_OP_FLOOR_DIVIDE, numerator, denominator));
	if (ratio < 1) check(-EINVAL);
	return ratio;
}

static mp_obj_t motor_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(port),
		PB_ARG_DEFAULT_OBJ(positive_direction, pb_Direction_CLOCKWISE_obj),
		PB_ARG_DEFAULT_NONE(gears), PB_ARG_DEFAULT_TRUE(reset_angle), PB_ARG_DEFAULT_NONE(profile));
	bool servo = type == &python_motor_type;
	if (!servo) mp_arg_parse_all(n_args, args, &kw_args, 2, allowed_args, parsed_args);
	int direction = pb_type_enum_get_value(positive_direction_in, &pb_enum_type_Direction);
	struct bp_servo_setup setup = {.direction = direction == PBIO_DIRECTION_CLOCKWISE ? 1 : -1,
		.gear_ratio = gear_ratio(gears_in), .reset_angle = mp_obj_is_true(reset_angle_in),
		.precision_profile = pb_obj_get_default_abs_int(profile_in, 0)};
	struct bp_port_state value;
	uint32_t port = python_port_connect(port_in, 0, &value);
	motor_obj_t *self = mp_obj_malloc(motor_obj_t, type);
	*self = (motor_obj_t){.base = {.type = type}, .port = port, .generation = value.generation};
	uint32_t request;
	for (;;) {
		int err = servo ? bp_servo_setup(port, self->generation, &setup, &request) :
			bp_motor_setup(port, self->generation, setup.direction, &request);
		if (err != -EAGAIN) {
			check(err);
			break;
		}
		mp_hal_delay_ms(2);
	}
	applied(self, request);
	return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t drive(motor_obj_t *self, int32_t duty, bool coast)
{
	if (self->closed) check(-ENODEV);
	uint32_t request;
	check(bp_motor_dc(self->port, self->generation, duty, coast, &request));
	applied(self, request);
	pb_type_async_schedule_stop_iteration(self->iter);
	return mp_const_none;
}

static mp_obj_t motor_dc(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_obj_t, self, PB_ARG_REQUIRED(duty));
	int duty = pb_obj_get_int(duty_in);
	return drive(self, CLAMP(duty, -100, 100) * 10, false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_dc_obj, 1, motor_dc);

static mp_obj_t motor_stop(mp_obj_t self) { return drive(MP_OBJ_TO_PTR(self), 0, true); }
static MP_DEFINE_CONST_FUN_OBJ_1(motor_stop_obj, motor_stop);
static mp_obj_t motor_brake(mp_obj_t self) { return drive(MP_OBJ_TO_PTR(self), 0, false); }
static MP_DEFINE_CONST_FUN_OBJ_1(motor_brake_obj, motor_brake);

static mp_obj_t motor_settings(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_obj_t, self, PB_ARG_DEFAULT_NONE(max_voltage));
	if (max_voltage_in == mp_const_none) {
		struct bp_motor_state value;
		configured(self, &value);
		mp_obj_t voltage = mp_obj_new_int(value.max_voltage);
		return mp_obj_new_tuple(1, &voltage);
	}
	if (self->closed) check(-ENODEV);
	check(bp_motor_set_voltage(self->port, self->generation, pb_obj_get_int(max_voltage_in)));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_settings_obj, 1, motor_settings);

static mp_obj_t motor_close(mp_obj_t self_in)
{
	motor_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (self->closed) return mp_const_none;
	check(bp_motor_close(self->port, self->generation));
	for (;;) {
		struct bp_motor_state value;
		snapshot(self, &value);
		if (!value.pending) {
			check(value.cleanup_error);
			if (!value.owner && value.valid && value.mode == BP_MOTOR_COAST) break;
		}
		mp_hal_delay_ms(2);
	}
	self->closed = true;
	pb_type_async_schedule_stop_iteration(self->iter);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(motor_close_obj, motor_close);

static struct bp_servo_state servo_state(motor_obj_t *self, uint32_t window)
{
	if (self->closed) check(-ENODEV);
	struct bp_servo_state value;
	check(bp_servo_get(self->port, self->generation, window, &value));
	return value;
}

static mp_obj_t motor_angle(mp_obj_t self)
{
	return mp_obj_new_int(servo_state(MP_OBJ_TO_PTR(self), 100).angle);
}
static MP_DEFINE_CONST_FUN_OBJ_1(motor_angle_obj, motor_angle);

static mp_obj_t motor_speed(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_obj_t, self, PB_ARG_DEFAULT_INT(window, 100));
	return mp_obj_new_int(servo_state(self, pb_obj_get_positive_int(window_in)).speed);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_speed_obj, 1, motor_speed);

static mp_obj_t motor_load(mp_obj_t self) { return mp_obj_new_int(servo_state(MP_OBJ_TO_PTR(self), 100).load); }
static MP_DEFINE_CONST_FUN_OBJ_1(motor_load_obj, motor_load);
static mp_obj_t motor_stalled(mp_obj_t self) { return mp_obj_new_bool(servo_state(MP_OBJ_TO_PTR(self), 100).stalled); }
static MP_DEFINE_CONST_FUN_OBJ_1(motor_stalled_obj, motor_stalled);
static mp_obj_t motor_done(mp_obj_t self) { return mp_obj_new_bool(servo_state(MP_OBJ_TO_PTR(self), 100).done); }
static MP_DEFINE_CONST_FUN_OBJ_1(motor_done_obj, motor_done);

static mp_obj_t motor_reset_angle(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_obj_t, self, PB_ARG_DEFAULT_NONE(angle));
	if (self->closed) check(-ENODEV);
	uint32_t request;
	bool absolute = angle_in == mp_const_none;
	check(bp_servo_reset(self->port, self->generation, absolute ? 0 : pb_obj_get_int(angle_in), absolute, &request));
	applied(self, request);
	pb_type_async_schedule_stop_iteration(self->iter);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_reset_angle_obj, 1, motor_reset_angle);

static mp_obj_t cancel_motion(mp_obj_t self_in)
{
	motor_obj_t *self = MP_OBJ_TO_PTR(self_in);
	struct bp_motor_state value;
	snapshot(self, &value);
	/* An alias can replace this object's motion. Closing its old awaitable
	 * must not coast the newer command. Generation is checked again in dc(). */
	if (value.request == self->motion_request) return motor_stop(self_in);
	return mp_const_none;
}

static void motion_error(motor_obj_t *self, void *error)
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
	motor_obj_t *self = MP_OBJ_TO_PTR(self_in);
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		/* Waiting only needs command status, not a copy/differentiation of the
		 * speed history on every poll. The worker owns feedback freshness. */
		struct bp_motor_state value;
		snapshot(self, &value);
		bool done = value.request != self->motion_request;
		if (!done) {
			check(value.error);
			if (!value.control.observing) check(-ENODEV);
			done = !value.pending && (!value.control.active || value.control.done);
		}
		nlr_pop();
		return done ? PBIO_SUCCESS : PBIO_ERROR_AGAIN;
	}
	motion_error(self, nlr.ret_val);
	return PBIO_SUCCESS; /* nlr_jump does not return. */
}

static mp_obj_t move(motor_obj_t *self, struct bp_servo_command command, bool wait, bool final_angle)
{
	if (self->closed) check(-ENODEV);
	uint32_t request;
	check(bp_servo_move(self->port, self->generation, &command, &request));
	self->motion_request = request;
	pb_type_async_schedule_stop_iteration(self->iter);
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		applied(self, request);
		mp_obj_t result = mp_const_none;
		if (wait) {
			pb_type_async_t config = {.parent_obj = MP_OBJ_FROM_PTR(self), .iter_once = motion_next,
				.close = cancel_motion, .return_map = final_angle ? motor_angle : NULL};
			if (pb_module_tools_run_loop_is_active()) result = pb_type_async_wait_or_await(&config, &self->iter, true);
			else {
				while (motion_next(NULL, MP_OBJ_FROM_PTR(self)) == PBIO_ERROR_AGAIN) mp_hal_delay_ms(2);
				if (final_angle) result = motor_angle(MP_OBJ_FROM_PTR(self));
			}
		}
		nlr_pop();
		return result;
	}
	/* An interrupted blocking wait coasts only its own command. Cleanup must
	 * not replace the original interruption or device error. */
	motion_error(self, nlr.ret_val);
	return mp_const_none; /* nlr_jump does not return. */
}

static mp_obj_t motor_run(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_obj_t, self, PB_ARG_REQUIRED(speed));
	return move(self, (struct bp_servo_command){.action = BP_SERVO_RUN, .speed = pb_obj_get_int(speed_in)}, false, false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_run_obj, 1, motor_run);

static mp_obj_t motor_run_time(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_obj_t, self, PB_ARG_REQUIRED(speed), PB_ARG_REQUIRED(time),
		PB_ARG_DEFAULT_OBJ(then, pb_Stop_HOLD_obj), PB_ARG_DEFAULT_TRUE(wait));
	return move(self, (struct bp_servo_command){.action = BP_SERVO_TIME, .speed = pb_obj_get_int(speed_in),
		.duration_ms = MAX(pb_obj_get_int(time_in), 0),
		.finish = pb_type_enum_get_value(then_in, &pb_enum_type_Stop)}, mp_obj_is_true(wait_in), false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_run_time_obj, 1, motor_run_time);

static mp_obj_t motor_run_angle(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_obj_t, self, PB_ARG_REQUIRED(speed), PB_ARG_REQUIRED(rotation_angle),
		PB_ARG_DEFAULT_OBJ(then, pb_Stop_HOLD_obj), PB_ARG_DEFAULT_TRUE(wait));
	return move(self, (struct bp_servo_command){.action = BP_SERVO_ANGLE, .speed = pb_obj_get_int(speed_in),
		.angle = pb_obj_get_int(rotation_angle_in), .finish = pb_type_enum_get_value(then_in, &pb_enum_type_Stop)},
		mp_obj_is_true(wait_in), false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_run_angle_obj, 1, motor_run_angle);

static mp_obj_t motor_run_target(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_obj_t, self, PB_ARG_REQUIRED(speed), PB_ARG_REQUIRED(target_angle),
		PB_ARG_DEFAULT_OBJ(then, pb_Stop_HOLD_obj), PB_ARG_DEFAULT_TRUE(wait));
	return move(self, (struct bp_servo_command){.action = BP_SERVO_TARGET, .speed = pb_obj_get_int(speed_in),
		.angle = pb_obj_get_int(target_angle_in), .finish = pb_type_enum_get_value(then_in, &pb_enum_type_Stop)},
		mp_obj_is_true(wait_in), false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_run_target_obj, 1, motor_run_target);

static mp_obj_t motor_track_target(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_obj_t, self, PB_ARG_REQUIRED(target_angle));
	return move(self, (struct bp_servo_command){.action = BP_SERVO_TRACK, .angle = pb_obj_get_int(target_angle_in)}, false, false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_track_target_obj, 1, motor_track_target);

static mp_obj_t motor_hold(mp_obj_t self)
{
	return move(MP_OBJ_TO_PTR(self), (struct bp_servo_command){.action = BP_SERVO_HOLD}, false, false);
}
static MP_DEFINE_CONST_FUN_OBJ_1(motor_hold_obj, motor_hold);

static mp_obj_t motor_run_until_stalled(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_obj_t, self, PB_ARG_REQUIRED(speed),
		PB_ARG_DEFAULT_OBJ(then, pb_Stop_COAST_obj), PB_ARG_DEFAULT_NONE(duty_limit));
	return move(self, (struct bp_servo_command){.action = BP_SERVO_UNTIL_STALLED, .speed = pb_obj_get_int(speed_in),
		.finish = pb_type_enum_get_value(then_in, &pb_enum_type_Stop),
		.duty_limit = duty_limit_in == mp_const_none ? -1 : pb_obj_get_pct(duty_limit_in) * 10}, true, true);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_run_until_stalled_obj, 1, motor_run_until_stalled);

#define DC_METHODS \
	{MP_ROM_QSTR(MP_QSTR_dc), MP_ROM_PTR(&motor_dc_obj)}, \
	{MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&motor_stop_obj)}, \
	{MP_ROM_QSTR(MP_QSTR_brake), MP_ROM_PTR(&motor_brake_obj)}, \
	{MP_ROM_QSTR(MP_QSTR_settings), MP_ROM_PTR(&motor_settings_obj)}, \
	{MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&motor_close_obj)}

static const mp_rom_map_elem_t dc_motor_methods[] = {DC_METHODS};
static MP_DEFINE_CONST_DICT(dc_motor_dict, dc_motor_methods);
MP_DEFINE_CONST_OBJ_TYPE(python_dc_motor_type, MP_QSTR_DCMotor, MP_TYPE_FLAG_NONE,
	make_new, motor_new, locals_dict, &dc_motor_dict);

static const mp_rom_map_elem_t motor_methods[] = {
	DC_METHODS,
	{MP_ROM_QSTR(MP_QSTR_angle), MP_ROM_PTR(&motor_angle_obj)},
	{MP_ROM_QSTR(MP_QSTR_speed), MP_ROM_PTR(&motor_speed_obj)},
	{MP_ROM_QSTR(MP_QSTR_reset_angle), MP_ROM_PTR(&motor_reset_angle_obj)},
	{MP_ROM_QSTR(MP_QSTR_load), MP_ROM_PTR(&motor_load_obj)},
	{MP_ROM_QSTR(MP_QSTR_stalled), MP_ROM_PTR(&motor_stalled_obj)},
	{MP_ROM_QSTR(MP_QSTR_done), MP_ROM_PTR(&motor_done_obj)},
	{MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&motor_run_obj)},
	{MP_ROM_QSTR(MP_QSTR_run_time), MP_ROM_PTR(&motor_run_time_obj)},
	{MP_ROM_QSTR(MP_QSTR_run_angle), MP_ROM_PTR(&motor_run_angle_obj)},
	{MP_ROM_QSTR(MP_QSTR_run_target), MP_ROM_PTR(&motor_run_target_obj)},
	{MP_ROM_QSTR(MP_QSTR_run_until_stalled), MP_ROM_PTR(&motor_run_until_stalled_obj)},
	{MP_ROM_QSTR(MP_QSTR_track_target), MP_ROM_PTR(&motor_track_target_obj)},
	{MP_ROM_QSTR(MP_QSTR_hold), MP_ROM_PTR(&motor_hold_obj)},
};
static MP_DEFINE_CONST_DICT(motor_dict, motor_methods);
MP_DEFINE_CONST_OBJ_TYPE(python_motor_type, MP_QSTR_Motor, MP_TYPE_FLAG_NONE,
	make_new, motor_new, locals_dict, &motor_dict);
