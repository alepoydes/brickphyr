// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
// Adapted from pybricks/hubs/pb_type_primehub.c and common/pb_type_{battery,charger}.c.
#include "devices.h"
#include <brickphyr.h>
#include <errno.h>
#include <pybricks/common.h>
#include <pybricks/hubs.h>
#include <pybricks/messaging/messaging.h>
#include <pybricks/parameters.h>
#include <pybricks/parameters/pb_type_button.h>
#include <pybricks/util_mp/pb_obj_helper.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <zephyr/devicetree.h>

typedef struct {
	mp_obj_base_t base;
	mp_obj_t buttons, system;
#ifdef CONFIG_BRICKPHYR_LIGHT
	mp_obj_t light;
#if DT_NODE_EXISTS(DT_NODELABEL(bluetooth_led))
	mp_obj_t bluetooth_light;
#endif
#endif
#ifdef CONFIG_BRICKPHYR_MATRIX
	mp_obj_t display;
#endif
#ifdef CONFIG_BRICKPHYR_BATTERY
	mp_obj_t battery;
#endif
#ifdef CONFIG_BRICKPHYR_CHARGER
	mp_obj_t charger;
#endif
#ifdef CONFIG_BRICKPHYR_SOUND
	mp_obj_t speaker;
#endif
#ifdef CONFIG_BRICKPHYR_IMU
	mp_obj_t imu;
#endif
#ifdef CONFIG_BRICKPHYR_RADIO
	mp_obj_t ble;
#endif
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

#ifdef CONFIG_BRICKPHYR_BATTERY
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

static mp_obj_t battery_type(void)
{
	struct bp_battery_sample sample;
	python_device_check(bp_battery_get(&sample));
	switch (sample.type) {
	case BP_BATTERY_ALKALINE:
		return MP_OBJ_NEW_QSTR(MP_QSTR_Alkaline);
	case BP_BATTERY_LI_ION:
		return MP_OBJ_NEW_QSTR(MP_QSTR_Li_hyphen_ion);
	default:
		return MP_OBJ_NEW_QSTR(MP_QSTR_Unknown);
	}
}
static MP_DEFINE_CONST_FUN_OBJ_0(battery_type_obj, battery_type);

static mp_obj_t battery_temperature(void)
{
	struct bp_battery_sample sample;
	python_device_check(bp_battery_get(&sample));
	python_device_check(sample.temperature_error);
	return mp_obj_new_int(sample.temperature_mdegc);
}
static MP_DEFINE_CONST_FUN_OBJ_0(battery_temperature_obj, battery_temperature);

static const mp_rom_map_elem_t battery_globals[] = {
	{ MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_battery) },
	{ MP_ROM_QSTR(MP_QSTR_voltage), MP_ROM_PTR(&battery_voltage_obj) },
	{ MP_ROM_QSTR(MP_QSTR_current), MP_ROM_PTR(&battery_current_obj) },
	{ MP_ROM_QSTR(MP_QSTR_type), MP_ROM_PTR(&battery_type_obj) },
	{ MP_ROM_QSTR(MP_QSTR_temperature), MP_ROM_PTR(&battery_temperature_obj) },
};
static MP_DEFINE_CONST_DICT(battery_dict, battery_globals);
static const mp_obj_module_t battery = {{&mp_type_module}, (mp_obj_dict_t *)&battery_dict};
#endif

#ifdef CONFIG_BRICKPHYR_CHARGER
/* pb_type_charger.c interface over the same cached native observations used
 * by Shell and telemetry. Acquisition errors must not masquerade as a status. */
static struct bp_charger_state charger_sample(void)
{
	struct bp_charger_state value;
	python_device_check(bp_charger_get(&value));
	python_device_check(value.error);
	return value;
}

static mp_obj_t charger_current(void)
{
	return mp_obj_new_int(charger_sample().current_ma);
}
static MP_DEFINE_CONST_FUN_OBJ_0(charger_current_obj, charger_current);

static mp_obj_t charger_status(void)
{
	return MP_OBJ_NEW_SMALL_INT(charger_sample().status);
}
static MP_DEFINE_CONST_FUN_OBJ_0(charger_status_obj, charger_status);

static mp_obj_t charger_connected(void)
{
	struct bp_charger_state value;
	python_device_check(bp_charger_get(&value));
	if (value.source < 0) python_device_check(value.source);
	return MP_OBJ_NEW_SMALL_INT(value.source);
}
static MP_DEFINE_CONST_FUN_OBJ_0(charger_connected_obj, charger_connected);

static const mp_rom_map_elem_t charger_globals[] = {
	{ MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_charger) },
	{ MP_ROM_QSTR(MP_QSTR_current), MP_ROM_PTR(&charger_current_obj) },
	{ MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&charger_status_obj) },
	{ MP_ROM_QSTR(MP_QSTR_connected), MP_ROM_PTR(&charger_connected_obj) },
};
static MP_DEFINE_CONST_DICT(charger_dict, charger_globals);
static const mp_obj_module_t charger = {{&mp_type_module}, (mp_obj_dict_t *)&charger_dict};
#endif

static mp_obj_t hub_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args,
		PB_ARG_DEFAULT_OBJ(top_side, pb_type_Axis_Z_obj),
		PB_ARG_DEFAULT_OBJ(front_side, pb_type_Axis_X_obj),
		PB_ARG_DEFAULT_NONE(broadcast_channel),
		PB_ARG_DEFAULT_OBJ(observe_channels, mp_const_empty_tuple_obj));
	if (broadcast_channel_in != mp_const_none) {
		mp_raise_NotImplementedError(MP_ERROR_TEXT("radio broadcasting"));
	}
	/* Reduced builds accept default construction, not unsupported configuration. */
#ifdef CONFIG_BRICKPHYR_IMU
	python_imu_axes(top_side_in, front_side_in);
#else
	if (top_side_in != MP_OBJ_FROM_PTR(&pb_type_Axis_Z_obj) ||
	    front_side_in != MP_OBJ_FROM_PTR(&pb_type_Axis_X_obj)) {
		mp_raise_OSError(MP_ENOTSUP);
	}
#endif
#ifndef CONFIG_BRICKPHYR_RADIO
	if (observe_channels_in != MP_OBJ_FROM_PTR(&mp_const_empty_tuple_obj)) {
		mp_raise_OSError(MP_ENOTSUP);
	}
#endif
	hub_obj_t *self = mp_obj_malloc(hub_obj_t, type);
	self->buttons = pb_type_Keypad_obj_new(MP_OBJ_FROM_PTR(self), buttons_pressed);
	self->system = MP_OBJ_FROM_PTR(&python_system);
#ifdef CONFIG_BRICKPHYR_LIGHT
	self->light = python_light_new(BP_LIGHT_CENTER);
#if DT_NODE_EXISTS(DT_NODELABEL(bluetooth_led))
	self->bluetooth_light = python_light_new(BP_LIGHT_BLUETOOTH);
#endif
#endif
#ifdef CONFIG_BRICKPHYR_MATRIX
	self->display = python_matrix_new();
#endif
#ifdef CONFIG_BRICKPHYR_BATTERY
	self->battery = MP_OBJ_FROM_PTR(&battery);
#endif
#ifdef CONFIG_BRICKPHYR_CHARGER
	self->charger = MP_OBJ_FROM_PTR(&charger);
#endif
#ifdef CONFIG_BRICKPHYR_SOUND
	self->speaker = python_speaker_new();
#endif
#ifdef CONFIG_BRICKPHYR_IMU
	self->imu = MP_OBJ_FROM_PTR(&python_imu);
#endif
#ifdef CONFIG_BRICKPHYR_RADIO
	self->ble = pb_type_BLE_new(broadcast_channel_in, observe_channels_in);
#endif
	return MP_OBJ_FROM_PTR(self);
}

static const pb_attr_dict_entry_t hub_attributes[] = {
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_buttons, hub_obj_t, buttons),
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_system, hub_obj_t, system),
#ifdef CONFIG_BRICKPHYR_LIGHT
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_light, hub_obj_t, light),
#if DT_NODE_EXISTS(DT_NODELABEL(bluetooth_led))
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_bluetooth_light, hub_obj_t, bluetooth_light),
#endif
#endif
#ifdef CONFIG_BRICKPHYR_MATRIX
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_display, hub_obj_t, display),
#endif
#ifdef CONFIG_BRICKPHYR_BATTERY
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_battery, hub_obj_t, battery),
#endif
#ifdef CONFIG_BRICKPHYR_CHARGER
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_charger, hub_obj_t, charger),
#endif
#ifdef CONFIG_BRICKPHYR_SOUND
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_speaker, hub_obj_t, speaker),
#endif
#ifdef CONFIG_BRICKPHYR_IMU
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_imu, hub_obj_t, imu),
#endif
#ifdef CONFIG_BRICKPHYR_RADIO
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_ble, hub_obj_t, ble),
#endif
	PB_ATTR_DICT_SENTINEL
};

MP_DEFINE_CONST_OBJ_TYPE(pb_type_ThisHub, MP_QSTR_PrimeHub, MP_TYPE_FLAG_NONE,
	make_new, hub_new, attr, pb_attribute_handler, protocol, hub_attributes);
