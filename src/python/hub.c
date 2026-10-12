// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
// Adapted from pybricks/hubs/pb_type_primehub.c and common/pb_type_battery.c.
#include "devices.h"
#include <brickphyr.h>
#include <pybricks/common.h>
#include <pybricks/hubs.h>
#include <pybricks/parameters.h>
#include <pybricks/parameters/pb_type_button.h>
#include <pybricks/util_mp/pb_obj_helper.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>

typedef struct {
	mp_obj_base_t base;
	mp_obj_t buttons, light, bluetooth_light, display, battery, speaker, system, imu;
} hub_obj_t;

static mp_obj_t buttons_pressed(mp_obj_t parent)
{
	uint32_t flags = bp_buttons();
	const qstr names[] = {MP_QSTR_CENTER, MP_QSTR_LEFT, MP_QSTR_RIGHT, MP_QSTR_BLUETOOTH};
	mp_obj_t pressed[MP_ARRAY_SIZE(names)];
	size_t count = 0;
	for (size_t i = 0; i < MP_ARRAY_SIZE(names); i++) {
		if (flags & (1U << i)) {
			pressed[count++] = pb_type_button_new(names[i]);
		}
	}
	return mp_obj_new_set(count, pressed);
}

static mp_obj_t battery_voltage(void)
{
	struct bp_battery_sample sample;
	python_device_check(bp_battery_get(&sample));
	python_device_check(sample.error);
	return mp_obj_new_int_from_uint(sample.voltage_mv);
}
static MP_DEFINE_CONST_FUN_OBJ_0(battery_voltage_obj, battery_voltage);

static mp_obj_t battery_current(void)
{
	struct bp_battery_sample sample;
	python_device_check(bp_battery_get(&sample));
	python_device_check(sample.error);
	return mp_obj_new_int_from_uint(sample.current_ma);
}
static MP_DEFINE_CONST_FUN_OBJ_0(battery_current_obj, battery_current);

static const mp_rom_map_elem_t battery_globals[] = {
	{ MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_battery) },
	{ MP_ROM_QSTR(MP_QSTR_voltage), MP_ROM_PTR(&battery_voltage_obj) },
	{ MP_ROM_QSTR(MP_QSTR_current), MP_ROM_PTR(&battery_current_obj) },
};
static MP_DEFINE_CONST_DICT(battery_dict, battery_globals);
static const mp_obj_module_t battery = {{&mp_type_module}, (mp_obj_dict_t *)&battery_dict};

static mp_obj_t hub_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args,
		PB_ARG_DEFAULT_OBJ(top_side, pb_type_Axis_Z_obj),
		PB_ARG_DEFAULT_OBJ(front_side, pb_type_Axis_X_obj));
	/* Default construction remains usable in a profile without an IMU. */
	if (IS_ENABLED(CONFIG_BRICKPHYR_IMU) || n_args || n_kw) {
		python_imu_axes(top_side_in, front_side_in);
	}
	hub_obj_t *self = mp_obj_malloc(hub_obj_t, type);
	self->buttons = pb_type_Keypad_obj_new(MP_OBJ_FROM_PTR(self), buttons_pressed);
	self->light = python_light_new(BP_LIGHT_CENTER);
	self->bluetooth_light = python_light_new(BP_LIGHT_BLUETOOTH);
	self->display = python_matrix_new();
	self->battery = MP_OBJ_FROM_PTR(&battery);
	self->speaker = python_speaker_new();
	self->system = MP_OBJ_FROM_PTR(&python_system);
	self->imu = MP_OBJ_FROM_PTR(&python_imu);
	return MP_OBJ_FROM_PTR(self);
}

static const pb_attr_dict_entry_t hub_attributes[] = {
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_buttons, hub_obj_t, buttons),
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_light, hub_obj_t, light),
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_bluetooth_light, hub_obj_t, bluetooth_light),
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_display, hub_obj_t, display),
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_battery, hub_obj_t, battery),
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_speaker, hub_obj_t, speaker),
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_system, hub_obj_t, system),
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_imu, hub_obj_t, imu),
	PB_ATTR_DICT_SENTINEL
};

MP_DEFINE_CONST_OBJ_TYPE(pb_type_ThisHub, MP_QSTR_PrimeHub, MP_TYPE_FLAG_NONE,
	make_new, hub_new, attr, pb_attribute_handler, protocol, hub_attributes);
