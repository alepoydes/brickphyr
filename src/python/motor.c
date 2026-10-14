// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
/* Adapted from pybricks/common/pb_type_motor.c at a7b1b04ae.
 * Direction, voltage limits and output ownership live in the native service. */
#include "devices.h"
#include "py/mphal.h"
#include <brickphyr.h>
#include <errno.h>
#include <string.h>
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
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	uint32_t motion_request;
	pb_type_async_t *iter;
	mp_obj_t control, model, log, control_log;
#endif
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

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
void python_motor_connection(mp_obj_t motor, uint32_t *port, uint32_t *generation)
{
	if (!mp_obj_is_type(motor, &python_motor_type)) mp_raise_TypeError(MP_ERROR_TEXT("expected Motor"));
	motor_obj_t *self = MP_OBJ_TO_PTR(motor);
	struct bp_motor_state value;
	configured(self, &value);
	*port = self->port;
	*generation = self->generation;
}
#endif

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

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
void python_motor_applied(mp_obj_t motor, uint32_t request)
{
	applied(MP_OBJ_TO_PTR(motor), request);
}
#endif

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
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
#endif

static mp_obj_t motor_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(port),
		PB_ARG_DEFAULT_OBJ(positive_direction, pb_Direction_CLOCKWISE_obj),
		PB_ARG_DEFAULT_NONE(gears), PB_ARG_DEFAULT_TRUE(reset_angle), PB_ARG_DEFAULT_NONE(profile));
	bool servo = type == &python_motor_type;
	if (!servo) mp_arg_parse_all(n_args, args, &kw_args, 2, allowed_args, parsed_args);
#else
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(port),
		PB_ARG_DEFAULT_OBJ(positive_direction, pb_Direction_CLOCKWISE_obj));
#endif
	int direction = pb_type_enum_get_value(positive_direction_in, &pb_enum_type_Direction);
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	struct bp_servo_setup setup = {.direction = direction == PBIO_DIRECTION_CLOCKWISE ? 1 : -1,
		.gear_ratio = gear_ratio(gears_in), .reset_angle = mp_obj_is_true(reset_angle_in),
		.precision_profile = pb_obj_get_default_abs_int(profile_in, 0)};
#endif
	struct bp_port_state value;
	uint32_t port = python_port_connect(port_in, 0, &value);
	motor_obj_t *self = mp_obj_malloc(motor_obj_t, type);
	*self = (motor_obj_t){.base = {.type = type}, .port = port, .generation = value.generation};
	uint32_t request;
	for (;;) {
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
		int err = servo ? bp_servo_setup(port, self->generation, &setup, &request) :
			bp_motor_setup(port, self->generation, setup.direction, &request);
#else
		int err = bp_motor_setup(port, self->generation,
			direction == PBIO_DIRECTION_CLOCKWISE ? 1 : -1, &request);
#endif
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
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	pb_type_async_schedule_stop_iteration(self->iter);
#endif
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
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	pb_type_async_schedule_stop_iteration(self->iter);
#endif
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(motor_close_obj, motor_close);

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
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
#endif

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

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
/* Adapted from pb_type_control.c at a7b1b04ae. Retain the Motor object rather
 * than a privileged PBIO pointer; aliases see the native port's live settings. */
typedef struct {
	mp_obj_base_t base;
	motor_obj_t *motor;
	mp_obj_t parent, log;
	uint32_t handle, axis, log_kind;
} motor_child_t;

static mp_obj_t motor_logger(motor_obj_t *motor, uint32_t kind);
static mp_obj_t pair_logger(motor_child_t *self);

static struct bp_servo_settings control_settings(motor_child_t *self)
{
	if (!self->motor) {
		struct bp_drivebase_axis_state value;
		check(bp_drivebase_axis_get(self->handle, self->axis, &value));
		return value.settings;
	}
	motor_obj_t *m = self->motor;
	if (m->closed) check(-ENODEV);
	struct bp_servo_settings value;
	check(bp_servo_settings_get(m->port, m->generation, &value));
	return value;
}

static mp_obj_t control_set(motor_child_t *self, uint32_t group, struct bp_servo_settings *value)
{
	if (!self->motor) {
		uint32_t request;
		check(bp_drivebase_axis_set(self->handle, self->axis, group, value, &request));
		for (;;) {
			struct bp_drivebase_state state;
			check(bp_drivebase_get(self->handle, &state));
			check(state.error);
			check(state.cleanup_error);
			if (state.request != request) check(-ECANCELED);
			if (!state.pending && state.completed == request) {
				check(state.command_error);
				return mp_const_none;
			}
			mp_hal_delay_ms(2);
		}
	}
	motor_obj_t *m = self->motor;
	uint32_t revision;
	check(bp_servo_settings_set(m->port, m->generation, group, value, &revision));
	for (;;) {
		struct bp_servo_settings current = control_settings(self);
		if (current.revision != revision) check(-ECANCELED);
		if (current.applied == revision) return mp_const_none;
		mp_hal_delay_ms(2);
	}
}

static mp_obj_t control_limits(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_child_t, self,
		PB_ARG_DEFAULT_NONE(speed), PB_ARG_DEFAULT_NONE(acceleration), PB_ARG_DEFAULT_NONE(torque));
	struct bp_servo_settings s = control_settings(self);
	if (PB_PARSE_ARGS_METHOD_ALL_NONE()) {
		mp_obj_t accel[] = {mp_obj_new_int(s.acceleration), mp_obj_new_int(s.deceleration)};
		mp_obj_t result[] = {mp_obj_new_int(s.speed), s.acceleration == s.deceleration ? accel[0] :
			mp_obj_new_tuple(2, accel), mp_obj_new_int(s.torque)};
		return mp_obj_new_tuple(3, result);
	}
	s.speed = pb_obj_get_default_abs_int(speed_in, s.speed);
	s.torque = pb_obj_get_default_abs_int(torque_in, s.torque);
	if (acceleration_in != mp_const_none) {
		if (pb_obj_is_array(acceleration_in)) {
			size_t count;
			mp_obj_t *values;
			mp_obj_get_array(acceleration_in, &count, &values);
			if (count != 2) check(-EINVAL);
			s.acceleration = pb_obj_get_int(values[0]);
			s.deceleration = pb_obj_get_int(values[1]);
		} else {
			s.acceleration = s.deceleration = pb_obj_get_int(acceleration_in);
		}
	}
	return control_set(self, BP_SERVO_LIMITS, &s);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(control_limits_obj, 1, control_limits);

static mp_obj_t control_pid(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_child_t, self,
		PB_ARG_DEFAULT_NONE(kp), PB_ARG_DEFAULT_NONE(ki), PB_ARG_DEFAULT_NONE(kd),
		PB_ARG_DEFAULT_NONE(integral_deadzone), PB_ARG_DEFAULT_NONE(integral_rate));
	struct bp_servo_settings s = control_settings(self);
	if (PB_PARSE_ARGS_METHOD_ALL_NONE()) {
		mp_obj_t result[] = {mp_obj_new_int(s.kp), mp_obj_new_int(s.ki), mp_obj_new_int(s.kd),
			mp_obj_new_int(s.integral_deadzone), mp_obj_new_int(s.integral_rate)};
		return mp_obj_new_tuple(5, result);
	}
	s.kp = pb_obj_get_default_abs_int(kp_in, s.kp);
	s.ki = pb_obj_get_default_abs_int(ki_in, s.ki);
	s.kd = pb_obj_get_default_abs_int(kd_in, s.kd);
	s.integral_deadzone = pb_obj_get_default_abs_int(integral_deadzone_in, s.integral_deadzone);
	s.integral_rate = pb_obj_get_default_abs_int(integral_rate_in, s.integral_rate);
	return control_set(self, BP_SERVO_PID, &s);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(control_pid_obj, 1, control_pid);

static mp_obj_t control_target_tolerances(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_child_t, self,
		PB_ARG_DEFAULT_NONE(speed), PB_ARG_DEFAULT_NONE(position));
	struct bp_servo_settings s = control_settings(self);
	if (PB_PARSE_ARGS_METHOD_ALL_NONE()) {
		mp_obj_t result[] = {mp_obj_new_int(s.target_speed), mp_obj_new_int(s.target_position)};
		return mp_obj_new_tuple(2, result);
	}
	s.target_speed = pb_obj_get_default_abs_int(speed_in, s.target_speed);
	s.target_position = pb_obj_get_default_abs_int(position_in, s.target_position);
	return control_set(self, BP_SERVO_TARGET_TOLERANCES, &s);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(control_target_tolerances_obj, 1, control_target_tolerances);

static mp_obj_t control_stall_tolerances(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_child_t, self,
		PB_ARG_DEFAULT_NONE(speed), PB_ARG_DEFAULT_NONE(time));
	struct bp_servo_settings s = control_settings(self);
	if (PB_PARSE_ARGS_METHOD_ALL_NONE()) {
		mp_obj_t result[] = {mp_obj_new_int(s.stall_speed), mp_obj_new_int_from_uint(s.stall_time)};
		return mp_obj_new_tuple(2, result);
	}
	s.stall_speed = pb_obj_get_default_abs_int(speed_in, s.stall_speed);
	s.stall_time = pb_obj_get_default_abs_int(time_in, s.stall_time);
	return control_set(self, BP_SERVO_STALL_TOLERANCES, &s);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(control_stall_tolerances_obj, 1, control_stall_tolerances);

static struct bp_servo_trajectory control_snapshot(mp_obj_t object)
{
	motor_child_t *self = MP_OBJ_TO_PTR(object);
	if (!self->motor) {
		struct bp_drivebase_axis_state value;
		check(bp_drivebase_axis_get(self->handle, self->axis, &value));
		return value.trajectory;
	}
	motor_obj_t *m = self->motor;
	if (m->closed) check(-ENODEV);
	struct bp_servo_trajectory value;
	check(bp_servo_trajectory_get(m->port, m->generation, &value));
	return value;
}

static mp_obj_t control_trajectory(mp_obj_t object)
{
	struct bp_servo_trajectory t = control_snapshot(object);
	if (!t.active) return mp_const_none;
	mp_obj_t points[13];
	for (size_t i = 0; i < ARRAY_SIZE(points); i++) points[i] = mp_obj_new_int(t.points[i]);
	return mp_obj_new_tuple(ARRAY_SIZE(points), points);
}
static MP_DEFINE_CONST_FUN_OBJ_1(control_trajectory_obj, control_trajectory);
static mp_obj_t control_done(mp_obj_t self) { return mp_obj_new_bool(control_snapshot(self).done); }
static MP_DEFINE_CONST_FUN_OBJ_1(control_done_obj, control_done);
static mp_obj_t control_stalled(mp_obj_t self) { return mp_obj_new_bool(control_snapshot(self).stalled); }
static MP_DEFINE_CONST_FUN_OBJ_1(control_stalled_obj, control_stalled);
static mp_obj_t control_load(mp_obj_t self) { return mp_obj_new_int(control_snapshot(self).load); }
static MP_DEFINE_CONST_FUN_OBJ_1(control_load_obj, control_load);

static void control_attr(mp_obj_t object, qstr name, mp_obj_t *dest)
{
	if (dest[0] != MP_OBJ_NULL) return;
	if (name == MP_QSTR_log) {
		motor_child_t *self = MP_OBJ_TO_PTR(object);
		dest[0] = self->motor ? motor_logger(self->motor, BP_CONTROL_LOG) : pair_logger(self);
	}
	else if (name == MP_QSTR_scale) dest[0] = mp_obj_new_int(control_settings(MP_OBJ_TO_PTR(object)).scale);
	else dest[1] = MP_OBJ_SENTINEL;
}

static const mp_rom_map_elem_t control_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_limits), MP_ROM_PTR(&control_limits_obj)},
	{MP_ROM_QSTR(MP_QSTR_pid), MP_ROM_PTR(&control_pid_obj)},
	{MP_ROM_QSTR(MP_QSTR_target_tolerances), MP_ROM_PTR(&control_target_tolerances_obj)},
	{MP_ROM_QSTR(MP_QSTR_stall_tolerances), MP_ROM_PTR(&control_stall_tolerances_obj)},
	{MP_ROM_QSTR(MP_QSTR_trajectory), MP_ROM_PTR(&control_trajectory_obj)},
	{MP_ROM_QSTR(MP_QSTR_done), MP_ROM_PTR(&control_done_obj)},
	{MP_ROM_QSTR(MP_QSTR_load), MP_ROM_PTR(&control_load_obj)},
	{MP_ROM_QSTR(MP_QSTR_stalled), MP_ROM_PTR(&control_stalled_obj)},
};
static MP_DEFINE_CONST_DICT(control_dict, control_methods);
static MP_DEFINE_CONST_OBJ_TYPE(control_type, MP_QSTR_Control, MP_TYPE_FLAG_NONE,
	attr, control_attr, locals_dict, &control_dict);

/* pb_type_motor_model.c's raw observer settings and shaft-state view. */
static mp_obj_t model_settings(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_child_t, self, PB_ARG_DEFAULT_NONE(values));
	struct bp_servo_settings s = control_settings(self);
	struct bp_motor_model_settings *m = &s.model;
	if (values_in == mp_const_none) {
		mp_obj_t values[] = {mp_obj_new_int(m->stall_speed), mp_obj_new_int_from_uint(m->stall_ticks),
			mp_obj_new_int(m->negligible_voltage), mp_obj_new_int(m->stall_ratio),
			mp_obj_new_int(m->gain_low), mp_obj_new_int(m->gain_high),
			mp_obj_new_int(m->gain_threshold), mp_obj_new_int(m->friction_cutoff)};
		return mp_obj_new_tuple(ARRAY_SIZE(values), values);
	}
	size_t count;
	mp_obj_t *values;
	mp_obj_get_array(values_in, &count, &values);
	if (count != 8) check(-EINVAL);
	*m = (struct bp_motor_model_settings){.stall_speed = mp_obj_get_int(values[0]),
		.stall_ticks = pb_obj_get_positive_int(values[1]), .negligible_voltage = mp_obj_get_int(values[2]),
		.stall_ratio = mp_obj_get_int(values[3]), .gain_low = mp_obj_get_int(values[4]),
		.gain_high = mp_obj_get_int(values[5]), .gain_threshold = mp_obj_get_int(values[6]),
		.friction_cutoff = mp_obj_get_int(values[7])};
	return control_set(self, BP_SERVO_MODEL, &s);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(model_settings_obj, 1, model_settings);

static mp_obj_t model_state(mp_obj_t object)
{
	motor_obj_t *m = ((motor_child_t *)MP_OBJ_TO_PTR(object))->motor;
	if (m->closed) check(-ENODEV);
	struct bp_motor_model_state s;
	check(bp_servo_model_get(m->port, m->generation, &s));
	mp_obj_t state[] = {mp_obj_new_float((mp_float_t)s.angle_mdeg / 1000),
		mp_obj_new_float((mp_float_t)s.speed_mdeg_s / 1000),
		mp_obj_new_float((mp_float_t)s.current_tenth_ma / 10), mp_obj_new_bool(s.stalled)};
	return mp_obj_new_tuple(ARRAY_SIZE(state), state);
}
static MP_DEFINE_CONST_FUN_OBJ_1(model_state_obj, model_state);
static const mp_rom_map_elem_t model_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_settings), MP_ROM_PTR(&model_settings_obj)},
	{MP_ROM_QSTR(MP_QSTR_state), MP_ROM_PTR(&model_state_obj)},
};
static MP_DEFINE_CONST_DICT(model_dict, model_methods);
static MP_DEFINE_CONST_OBJ_TYPE(model_type, MP_QSTR_MotorModel, MP_TYPE_FLAG_NONE, locals_dict, &model_dict);

/* pb_type_logger.c's Python interface/CSV framing. Collection stays in the
 * native port worker; no background write ever targets the Python heap. */
static mp_obj_t motor_log_start(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_child_t, self,
		PB_ARG_REQUIRED(duration), PB_ARG_DEFAULT_INT(down_sample, 1));
	motor_obj_t *m = self->motor;
	uint32_t duration = pb_obj_get_positive_int(duration_in), down_sample = MAX(pb_obj_get_int(down_sample_in), 1);
	if (m && m->closed) check(-ENODEV);
	check(m ? bp_motor_log_start(m->port, m->generation, self->log_kind, duration, down_sample) :
		bp_drivebase_log_start(self->handle, self->axis, duration, down_sample));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_log_start_obj, 1, motor_log_start);

static mp_obj_t motor_log_stop(mp_obj_t object)
{
	motor_child_t *self = MP_OBJ_TO_PTR(object);
	motor_obj_t *m = self->motor;
	struct bp_motor_log_status status;
	check(m ? bp_motor_log_status(m->port, m->generation, self->log_kind, true, &status) :
		bp_drivebase_log_status(self->handle, self->axis, true, &status));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(motor_log_stop_obj, motor_log_stop);

static mp_obj_t motor_log_save(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, motor_child_t, self, PB_ARG_DEFAULT_NONE(path));
	const char *path = path_in == mp_const_none ? "log.txt" : mp_obj_str_get_str(path_in);
	if (strchr(path, '\n') || strchr(path, '\r')) check(-EINVAL);
	motor_obj_t *m = self->motor;
	struct bp_motor_log_status status;
	check(m ? bp_motor_log_status(m->port, m->generation, self->log_kind, true, &status) :
		bp_drivebase_log_status(self->handle, self->axis, true, &status));
	mp_printf(&mp_plat_print, "PB_OF:%s\n", path);
	int err = 0;
	for (uint32_t row = 0; row < status.rows; row++) {
		struct bp_motor_log_row data;
		err = m ? bp_motor_log_read(m->port, m->generation, self->log_kind, status.revision, row, &data) :
			bp_drivebase_log_read(self->handle, self->axis, status.revision, row, &data);
		if (err) break;
		for (size_t col = 0; col < status.columns; col++) {
			mp_printf(&mp_plat_print, col + 1 < status.columns ? "%d, " : "%d\n", data.values[col]);
		}
		mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
	}
	mp_print_str(&mp_plat_print, "PB_EOF\n");
	check(err ? err : status.error);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(motor_log_save_obj, 1, motor_log_save);
static const mp_rom_map_elem_t motor_log_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&motor_log_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&motor_log_stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_save), MP_ROM_PTR(&motor_log_save_obj)},
};
static MP_DEFINE_CONST_DICT(motor_log_dict, motor_log_methods);
static MP_DEFINE_CONST_OBJ_TYPE(motor_log_type, MP_QSTR_Logger, MP_TYPE_FLAG_NONE, locals_dict, &motor_log_dict);

mp_obj_t python_drivebase_control_new(mp_obj_t parent, uint32_t handle, uint32_t axis)
{
	motor_child_t *child = mp_obj_malloc(motor_child_t, &control_type);
	*child = (motor_child_t){.base = {.type = &control_type}, .parent = parent, .handle = handle, .axis = axis};
	return MP_OBJ_FROM_PTR(child);
}

static mp_obj_t pair_logger(motor_child_t *self)
{
	if (self->log == MP_OBJ_NULL) {
		motor_child_t *child = mp_obj_malloc(motor_child_t, &motor_log_type);
		*child = (motor_child_t){.base = {.type = &motor_log_type}, .parent = self->parent,
			.handle = self->handle, .axis = self->axis};
		self->log = MP_OBJ_FROM_PTR(child);
	}
	return self->log;
}

static mp_obj_t motor_logger(motor_obj_t *motor, uint32_t kind)
{
	mp_obj_t *slot = kind == BP_MOTOR_LOG ? &motor->log : &motor->control_log;
	if (*slot == MP_OBJ_NULL) {
		motor_child_t *child = mp_obj_malloc(motor_child_t, &motor_log_type);
		*child = (motor_child_t){.base = {.type = &motor_log_type}, .motor = motor, .log_kind = kind};
		*slot = MP_OBJ_FROM_PTR(child);
	}
	return *slot;
}

static void motor_attr(mp_obj_t object, qstr name, mp_obj_t *dest)
{
	motor_obj_t *self = MP_OBJ_TO_PTR(object);
	if (dest[0] != MP_OBJ_NULL) return;
	mp_obj_t *slot;
	const mp_obj_type_t *type;
	if (name == MP_QSTR_control) { slot = &self->control; type = &control_type; }
	else if (name == MP_QSTR_model) { slot = &self->model; type = &model_type; }
	else if (name == MP_QSTR_log) { dest[0] = motor_logger(self, BP_MOTOR_LOG); return; }
	else {
		dest[1] = MP_OBJ_SENTINEL;
		return;
	}
	if (*slot == MP_OBJ_NULL) {
		motor_child_t *child = mp_obj_malloc(motor_child_t, type);
		*child = (motor_child_t){.base = {.type = type}, .motor = self};
		*slot = MP_OBJ_FROM_PTR(child);
	}
	dest[0] = *slot;
}

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
	make_new, motor_new, attr, motor_attr, locals_dict, &motor_dict);
#endif
