// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
/* Adapted from pybricks/pupdevices sensor bindings at a7b1b04ae.
 * Units and Python results are mapped here; calibration, mode readiness and
 * conversion remain in the shared native port service. */
#include "devices.h"
#include "py/mphal.h"
#include <brickphyr.h>
#include <errno.h>
#include <math.h>
#include <lego/device.h>
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>

enum reading { FORCE, TRAVEL, TOUCHED, PRESSED, TILT, DISTANCE, REFLECTION, COUNT };
typedef struct {
	mp_obj_base_t base;
	pb_type_async_t *iter;
	uint32_t port, generation, measurement;
	uint16_t id;
	enum reading reading;
	int32_t count_offset, pressed_threshold;
} sensor_obj_t;
static const mp_obj_type_t force_type, tilt_type, infrared_type;

static void sample(sensor_obj_t *self, struct bp_sensor_sample *value)
{
	python_device_check(bp_sensor_get(self->port, value));
	if (value->generation != self->generation) python_device_check(-ENODEV);
	if (value->measurement != self->measurement) python_device_check(-ECANCELED);
}

static pbio_error_t ready(pbio_os_state_t *state, mp_obj_t self_in)
{
	sensor_obj_t *self = MP_OBJ_TO_PTR(self_in);
	struct bp_sensor_sample value;
	sample(self, &value);
	if (value.error == -EAGAIN) return PBIO_ERROR_AGAIN;
	python_device_check(value.error);
	return PBIO_SUCCESS;
}

static void select_measurement(sensor_obj_t *self, uint32_t measurement)
{
	if (self->iter && self->iter->parent_obj != MP_OBJ_NULL) python_device_check(-EBUSY);
	int err = bp_sensor_select(self->port, self->generation, measurement);
	python_device_check(err == -ESTALE ? -ENODEV : err);
	self->measurement = measurement;
}

static void wait_ready(sensor_obj_t *self)
{
	pbio_os_state_t state = {0};
	while (ready(&state, MP_OBJ_FROM_PTR(self)) == PBIO_ERROR_AGAIN) mp_hal_delay_ms(2);
}

static mp_obj_t result(mp_obj_t self_in)
{
	sensor_obj_t *self = MP_OBJ_TO_PTR(self_in);
	struct bp_sensor_sample value;
	sample(self, &value);
	python_device_check(value.error);
	switch (self->reading) {
	case FORCE: return pb_obj_new_fraction(value.force_mn, 1000);
	case TRAVEL: return pb_obj_new_fraction(value.travel_um, 1000);
	case TOUCHED: return mp_obj_new_bool(value.travel_um > 100);
	case PRESSED: return mp_obj_new_bool(value.force_mn > self->pressed_threshold);
	case TILT: {
		mp_obj_t values[] = {mp_obj_new_int(value.tilt_deg[0]), mp_obj_new_int(value.tilt_deg[1])};
		return mp_obj_new_tuple(MP_ARRAY_SIZE(values), values);
	}
	case DISTANCE: return mp_obj_new_int(value.distance_pct);
	case REFLECTION: return pb_obj_new_fraction(value.reflection_permille, 10);
	case COUNT: return mp_obj_new_int_from_ll((int64_t)value.count - self->count_offset);
	}
	python_device_check(-ENOTSUP);
	return mp_const_none;
}

static mp_obj_t read_measurement(mp_obj_t self_in, uint32_t measurement, enum reading reading)
{
	sensor_obj_t *self = MP_OBJ_TO_PTR(self_in);
	select_measurement(self, measurement);
	self->reading = reading;
	pb_type_async_t config = {.parent_obj = self_in, .iter_once = ready, .return_map = result};
	if (pb_module_tools_run_loop_is_active()) {
		return pb_type_async_wait_or_await(&config, &self->iter, false);
	}
	wait_ready(self);
	return result(self_in);
}

static mp_obj_t sensor_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(port));
	uint16_t id = type == &force_type ? LEGO_DEVICE_TYPE_ID_SPIKE_FORCE_SENSOR :
		type == &tilt_type ? LEGO_DEVICE_TYPE_ID_WEDO2_TILT_SENSOR : LEGO_DEVICE_TYPE_ID_WEDO2_MOTION_SENSOR;
	struct bp_port_state value;
	uint32_t port = python_port_connect(port_in, id, &value);
	sensor_obj_t *self = mp_obj_malloc(sensor_obj_t, type);
	*self = (sensor_obj_t){.base = {.type = type}, .port = port, .generation = value.generation, .id = id};
	if (type == &infrared_type) {
		/* Reference COUNT -> CAL initialization; the counter itself is untouched. */
		select_measurement(self, BP_SENSOR_COUNT);
		wait_ready(self);
		struct bp_sensor_sample initial;
		sample(self, &initial);
		python_device_check(initial.error);
		self->count_offset = initial.count;
	}
	select_measurement(self, type == &force_type ? BP_SENSOR_FORCE :
		type == &tilt_type ? BP_SENSOR_TILT : BP_SENSOR_DISTANCE);
	wait_ready(self);
	return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t force(mp_obj_t self) { return read_measurement(self, BP_SENSOR_FORCE, FORCE); }
static MP_DEFINE_CONST_FUN_OBJ_1(force_obj, force);
static mp_obj_t touched(mp_obj_t self) { return read_measurement(self, BP_SENSOR_FORCE, TOUCHED); }
static MP_DEFINE_CONST_FUN_OBJ_1(touched_obj, touched);
static mp_obj_t distance(mp_obj_t self_in)
{
	sensor_obj_t *self = MP_OBJ_TO_PTR(self_in);
	return self->id == LEGO_DEVICE_TYPE_ID_SPIKE_FORCE_SENSOR ?
		read_measurement(self_in, BP_SENSOR_FORCE, TRAVEL) : read_measurement(self_in, BP_SENSOR_DISTANCE, DISTANCE);
}
static MP_DEFINE_CONST_FUN_OBJ_1(distance_obj, distance);
static mp_obj_t pressed(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, sensor_obj_t, self, PB_ARG_DEFAULT_INT(force, 3));
	mp_float_t threshold = mp_obj_get_float(force_in) * 1000;
	if (!isfinite(threshold) || threshold < INT32_MIN || threshold >= 2147483648.0f) {
		mp_raise_ValueError(MP_ERROR_TEXT("invalid force threshold"));
	}
	if (self->iter && self->iter->parent_obj != MP_OBJ_NULL) python_device_check(-EBUSY);
	self->pressed_threshold = (int32_t)threshold;
	return read_measurement(MP_OBJ_FROM_PTR(self), BP_SENSOR_FORCE, PRESSED);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pressed_obj, 1, pressed);
static mp_obj_t tilt(mp_obj_t self) { return read_measurement(self, BP_SENSOR_TILT, TILT); }
static MP_DEFINE_CONST_FUN_OBJ_1(tilt_obj, tilt);
static mp_obj_t count(mp_obj_t self) { return read_measurement(self, BP_SENSOR_COUNT, COUNT); }
static MP_DEFINE_CONST_FUN_OBJ_1(count_obj, count);
static mp_obj_t reflection(mp_obj_t self) { return read_measurement(self, BP_SENSOR_SURFACE, REFLECTION); }
static MP_DEFINE_CONST_FUN_OBJ_1(reflection_obj, reflection);

static const mp_rom_map_elem_t force_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_force), MP_ROM_PTR(&force_obj)},
	{MP_ROM_QSTR(MP_QSTR_distance), MP_ROM_PTR(&distance_obj)},
	{MP_ROM_QSTR(MP_QSTR_touched), MP_ROM_PTR(&touched_obj)},
	{MP_ROM_QSTR(MP_QSTR_pressed), MP_ROM_PTR(&pressed_obj)},
};
static MP_DEFINE_CONST_DICT(force_dict, force_methods);
static MP_DEFINE_CONST_OBJ_TYPE(force_type, MP_QSTR_ForceSensor, MP_TYPE_FLAG_NONE,
	make_new, sensor_new, locals_dict, &force_dict);
static const mp_rom_map_elem_t tilt_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_tilt), MP_ROM_PTR(&tilt_obj)},
};
static MP_DEFINE_CONST_DICT(tilt_dict, tilt_methods);
static MP_DEFINE_CONST_OBJ_TYPE(tilt_type, MP_QSTR_TiltSensor, MP_TYPE_FLAG_NONE,
	make_new, sensor_new, locals_dict, &tilt_dict);
static const mp_rom_map_elem_t infrared_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_distance), MP_ROM_PTR(&distance_obj)},
	{MP_ROM_QSTR(MP_QSTR_reflection), MP_ROM_PTR(&reflection_obj)},
	{MP_ROM_QSTR(MP_QSTR_count), MP_ROM_PTR(&count_obj)},
};
static MP_DEFINE_CONST_DICT(infrared_dict, infrared_methods);
static MP_DEFINE_CONST_OBJ_TYPE(infrared_type, MP_QSTR_InfraredSensor, MP_TYPE_FLAG_NONE,
	make_new, sensor_new, locals_dict, &infrared_dict);
static const mp_rom_map_elem_t pupdevices_globals[] = {
	{MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_pupdevices)},
	{MP_ROM_QSTR(MP_QSTR_ForceSensor), MP_ROM_PTR(&force_type)},
	{MP_ROM_QSTR(MP_QSTR_TiltSensor), MP_ROM_PTR(&tilt_type)},
	{MP_ROM_QSTR(MP_QSTR_InfraredSensor), MP_ROM_PTR(&infrared_type)},
};
static MP_DEFINE_CONST_DICT(pupdevices_dict, pupdevices_globals);
const mp_obj_module_t pb_module_pupdevices = {{&mp_type_module}, (mp_obj_dict_t *)&pupdevices_dict};
