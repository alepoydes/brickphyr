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
#include <pybricks/parameters.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>
#include <pybricks/util_mp/pb_type_enum.h>

typedef struct {
	mp_obj_base_t base;
	uint32_t port, generation;
	bool closed;
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

static mp_obj_t motor_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(port),
		PB_ARG_DEFAULT_OBJ(positive_direction, pb_Direction_CLOCKWISE_obj));
	int direction = pb_type_enum_get_value(positive_direction_in, &pb_enum_type_Direction);
	struct bp_port_state value;
	uint32_t port = python_port_connect(port_in, 0, &value);
	motor_obj_t *self = mp_obj_malloc(motor_obj_t, type);
	*self = (motor_obj_t){.base = {.type = type}, .port = port, .generation = value.generation};
	uint32_t request;
	for (;;) {
		int err = bp_motor_setup(port, self->generation, direction == PBIO_DIRECTION_CLOCKWISE ? 1 : -1, &request);
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
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(motor_close_obj, motor_close);

static const mp_rom_map_elem_t motor_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_dc), MP_ROM_PTR(&motor_dc_obj)},
	{MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&motor_stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_brake), MP_ROM_PTR(&motor_brake_obj)},
	{MP_ROM_QSTR(MP_QSTR_settings), MP_ROM_PTR(&motor_settings_obj)},
	{MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&motor_close_obj)},
};
static MP_DEFINE_CONST_DICT(motor_dict, motor_methods);
MP_DEFINE_CONST_OBJ_TYPE(python_dc_motor_type, MP_QSTR_DCMotor, MP_TYPE_FLAG_NONE,
	make_new, motor_new, locals_dict, &motor_dict);
