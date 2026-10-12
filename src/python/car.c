// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
/* Adapted from pybricks/robotics/pb_type_car.c at a7b1b04ae.
 * Steering calibration and RC power mapping use the shared motor service. */
#include "devices.h"
#include "py/mphal.h"
#include <brickphyr.h>
#include <errno.h>
#include <zephyr/sys/util.h>
#include <pybricks/tools.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>

struct car_motor {
	mp_obj_t object;
	uint32_t port, generation, request;
};

typedef struct {
	mp_obj_base_t base;
	size_t count;
	int32_t max_angle;
	struct car_motor motors[BP_PORT_COUNT]; /* Steering first, then drive motors. */
} car_obj_t;

static void connections(car_obj_t *self)
{
	for (size_t i = 0; i < self->count; i++) {
		struct car_motor *motor = &self->motors[i];
		python_motor_connection(motor->object, &motor->port, &motor->generation);
	}
}

static void failed(car_obj_t *self, void *error)
{
	/* Queue all stops before waiting. Never cancel a newer command issued
	 * through another Motor/robot object, or replace the original exception. */
	uint32_t stops[BP_PORT_COUNT] = {0};
	for (size_t i = 0; i < self->count; i++) {
		struct car_motor *motor = &self->motors[i];
		struct bp_motor_state value;
		if (motor->request && !bp_motor_get(motor->port, &value) &&
		    value.generation == motor->generation && value.request == motor->request) {
			(void)bp_motor_dc(motor->port, motor->generation, 0, true, &stops[i]);
		}
		motor->request = 0;
	}
	for (size_t i = 0; i < self->count; i++) {
		if (!stops[i]) continue;
		nlr_buf_t cleanup;
		if (nlr_push(&cleanup) == 0) {
			python_motor_applied(self->motors[i].object, stops[i]);
			nlr_pop();
		}
	}
	nlr_jump(error);
}

static void move(struct car_motor *motor, struct bp_servo_command command)
{
	python_device_check(bp_servo_move(motor->port, motor->generation, &command, &motor->request));
	python_motor_applied(motor->object, motor->request);
}

static int32_t end_stop(struct car_motor *motor, int32_t speed, int32_t torque_limit)
{
	move(motor, (struct bp_servo_command){.action = BP_SERVO_UNTIL_STALLED_TORQUE,
		.speed = speed, .torque_limit = torque_limit, .finish = BP_MOTOR_FINISH_COAST});
	uint32_t start = mp_hal_ticks_ms();
	for (;;) {
		struct bp_servo_state value;
		python_device_check(bp_servo_get(motor->port, motor->generation, 100, &value));
		if (value.request != motor->request) python_device_check(-ECANCELED);
		if (value.done) return value.angle;
		if ((uint32_t)(mp_hal_ticks_ms() - start) > 10000) {
			mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("The steering mechanism has no end stop. Did you build a car yet?"));
		}
		mp_hal_delay_ms(10);
	}
}

static mp_obj_t car_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(steer_motor),
		PB_ARG_REQUIRED(drive_motors), PB_ARG_DEFAULT_INT(torque_limit, 100));
	pb_module_tools_assert_blocking();
	int32_t torque_limit = torque_limit_in == mp_const_none ? 1000 : pb_obj_get_pct(torque_limit_in) * 10;
	size_t count = 1;
	mp_obj_t *motors = &drive_motors_in;
	if (pb_obj_is_array(drive_motors_in)) mp_obj_get_array(drive_motors_in, &count, &motors);
	if (!count || count >= BP_PORT_COUNT) {
		mp_raise_ValueError(MP_ERROR_TEXT("drive_motors must contain 1 to 5 motors"));
	}
	car_obj_t *self = mp_obj_malloc(car_obj_t, type);
	*self = (car_obj_t){.base = {.type = type}, .count = count + 1};
	self->motors[0].object = steer_motor_in;
	for (size_t i = 0; i < count; i++) self->motors[i + 1].object = motors[i];
	connections(self);
	for (size_t i = 0; i < self->count; i++) {
		for (size_t j = 0; j < i; j++) {
			if (self->motors[i].port == self->motors[j].port) {
				mp_raise_ValueError(MP_ERROR_TEXT("steering and drive motors must use distinct ports"));
			}
		}
	}
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		struct car_motor *steering = &self->motors[0];
		int32_t left = end_stop(steering, -300, torque_limit);
		int32_t right = end_stop(steering, 300, torque_limit);
		int64_t half_range = ((int64_t)right - left) / 2;
		/* A 10-degree margin cannot fit a reversed or narrower mechanism. */
		if (half_range < 10 || half_range > INT32_MAX) {
			mp_raise_ValueError(MP_ERROR_TEXT("steering range is too small or invalid"));
		}
		self->max_angle = half_range;
		connections(self);
		python_device_check(bp_servo_reset(steering->port, steering->generation,
			self->max_angle, false, &steering->request));
		python_motor_applied(steering->object, steering->request);
		move(steering, (struct bp_servo_command){.action = BP_SERVO_TRACK});
		nlr_pop();
		return MP_OBJ_FROM_PTR(self);
	}
	failed(self, nlr.ret_val);
	return mp_const_none;
}

static mp_obj_t drive(car_obj_t *self, int32_t value, bool power)
{
	bool coast = power && value > -10 && value < 10;
	int32_t duty = power && !coast ? (value * 8 / 10 + (value > 0 ? 20 : -20)) * 10 : 0;
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		connections(self);
		for (size_t i = 1; i < self->count; i++) {
			struct car_motor *motor = &self->motors[i];
			if (power) {
				python_device_check(bp_motor_dc(motor->port, motor->generation, duty, coast, &motor->request));
				python_motor_applied(motor->object, motor->request);
			} else move(motor, (struct bp_servo_command){.action = BP_SERVO_RUN, .speed = value});
		}
		nlr_pop();
		return mp_const_none;
	}
	failed(self, nlr.ret_val);
	return mp_const_none;
}

static mp_obj_t drive_power(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, car_obj_t, self, PB_ARG_REQUIRED(power));
	return drive(self, CLAMP(pb_obj_get_int(power_in), -100, 100), true);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(drive_power_obj, 1, drive_power);

static mp_obj_t drive_speed(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, car_obj_t, self, PB_ARG_REQUIRED(speed));
	return drive(self, pb_obj_get_int(speed_in), false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(drive_speed_obj, 1, drive_speed);

static mp_obj_t steer(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, car_obj_t, self, PB_ARG_REQUIRED(percentage));
	int32_t percentage = CLAMP(pb_obj_get_int(percentage_in), -100, 100);
	int32_t angle = (int64_t)(self->max_angle - 10) * percentage / 100;
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		connections(self);
		move(&self->motors[0], (struct bp_servo_command){.action = BP_SERVO_TRACK, .angle = angle});
		nlr_pop();
		return mp_const_none;
	}
	failed(self, nlr.ret_val);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(steer_obj, 1, steer);

static const mp_rom_map_elem_t car_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_drive_power), MP_ROM_PTR(&drive_power_obj)},
	{MP_ROM_QSTR(MP_QSTR_drive_speed), MP_ROM_PTR(&drive_speed_obj)},
	{MP_ROM_QSTR(MP_QSTR_steer), MP_ROM_PTR(&steer_obj)},
};
static MP_DEFINE_CONST_DICT(car_dict, car_methods);
MP_DEFINE_CONST_OBJ_TYPE(python_car_type, MP_QSTR_Car, MP_TYPE_FLAG_NONE,
	make_new, car_new, locals_dict, &car_dict);
