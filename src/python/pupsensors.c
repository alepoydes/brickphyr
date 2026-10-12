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
#include <pybricks/common.h>
#include <pybricks/parameters.h>
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>

enum reading { FORCE, TRAVEL, TOUCHED, PRESSED, TILT, DISTANCE, REFLECTION, COUNT,
	AMBIENT, HSV, COLOR, PRESENCE, WRITE };
typedef struct {
	mp_obj_base_t base;
	pb_type_async_t *iter;
	uint32_t port, generation, measurement, write_request;
	uint16_t id;
	enum reading reading;
	int32_t count_offset, pressed_threshold;
	mp_obj_t light;
} sensor_obj_t;
static const mp_obj_type_t force_type, tilt_type, infrared_type, color_type, ultrasonic_type, color_distance_type;
static mp_obj_t light_new(mp_obj_t parent);
static mp_obj_t color_light_on(mp_obj_t parent, pbio_color_t color);

static void check_connection(int error)
{
	python_device_check(error == -ESTALE ? -ENODEV : error);
}

static void idle(sensor_obj_t *self)
{
	if (self->iter && self->iter->parent_obj != MP_OBJ_NULL) python_device_check(-EBUSY);
}

static void sample(sensor_obj_t *self, struct bp_sensor_sample *value)
{
	python_device_check(bp_sensor_get(self->port, value));
	if (value->generation != self->generation) python_device_check(-ENODEV);
	if (value->measurement != self->measurement) python_device_check(-ECANCELED);
}

static pbio_error_t ready(pbio_os_state_t *state, mp_obj_t self_in)
{
	sensor_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (self->reading == WRITE) {
		return python_port_write_done(self->port, self->generation, self->write_request) ? PBIO_SUCCESS : PBIO_ERROR_AGAIN;
	}
	struct bp_sensor_sample value;
	sample(self, &value);
	if (value.error == -EAGAIN) return PBIO_ERROR_AGAIN;
	python_device_check(value.error);
	return PBIO_SUCCESS;
}

static void select_measurement(sensor_obj_t *self, uint32_t measurement)
{
	idle(self);
	check_connection(bp_sensor_select(self->port, self->generation, measurement));
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
	if (self->reading == WRITE) return mp_const_none;
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
	case DISTANCE: return mp_obj_new_int(self->id == LEGO_DEVICE_TYPE_ID_SPIKE_ULTRASONIC_SENSOR ?
		value.distance_mm : value.distance_pct);
	case REFLECTION: return pb_obj_new_fraction(value.reflection_permille, 10);
	case COUNT: return mp_obj_new_int_from_ll((int64_t)value.count - self->count_offset);
	case AMBIENT: return pb_obj_new_fraction(value.ambient_permille, 10);
	case PRESENCE: return mp_obj_new_bool(value.presence);
	case HSV: return pb_type_Color_new(PBIO_COLOR_ENCODE(value.hue_deg, value.saturation_pct, value.value_pct));
	case COLOR: return value.fields & BP_SENSOR_HAS_MATCHED_COLOR ?
		pb_type_Color_from_hsv(value.matched_color) : mp_const_none;
	case WRITE: break;
	}
	python_device_check(-ENOTSUP);
	return mp_const_none;
}

static mp_obj_t wait_or_await(mp_obj_t self_in)
{
	sensor_obj_t *self = MP_OBJ_TO_PTR(self_in);
	pb_type_async_t config = {.parent_obj = self_in, .iter_once = ready, .return_map = result};
	if (pb_module_tools_run_loop_is_active()) {
		return pb_type_async_wait_or_await(&config, &self->iter, false);
	}
	wait_ready(self);
	return result(self_in);
}

static mp_obj_t read_measurement(mp_obj_t self_in, uint32_t measurement, enum reading reading)
{
	sensor_obj_t *self = MP_OBJ_TO_PTR(self_in);
	select_measurement(self, measurement);
	self->reading = reading;
	return wait_or_await(self_in);
}

static mp_obj_t sensor_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(port));
	uint16_t id = type == &force_type ? LEGO_DEVICE_TYPE_ID_SPIKE_FORCE_SENSOR :
		type == &tilt_type ? LEGO_DEVICE_TYPE_ID_WEDO2_TILT_SENSOR :
		type == &infrared_type ? LEGO_DEVICE_TYPE_ID_WEDO2_MOTION_SENSOR :
		type == &color_type ? LEGO_DEVICE_TYPE_ID_SPIKE_COLOR_SENSOR :
		type == &ultrasonic_type ? LEGO_DEVICE_TYPE_ID_SPIKE_ULTRASONIC_SENSOR : LEGO_DEVICE_TYPE_ID_COLOR_DIST_SENSOR;
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
		type == &tilt_type ? BP_SENSOR_TILT : type == &color_type || type == &color_distance_type ?
		BP_SENSOR_SURFACE : BP_SENSOR_DISTANCE);
	wait_ready(self);
	if (type == &color_type || type == &color_distance_type) {
		check_connection(bp_sensor_palette_set(port, self->generation, NULL));
	}
	if (type == &color_type || type == &ultrasonic_type) self->light = light_new(MP_OBJ_FROM_PTR(self));
	if (type == &color_distance_type) {
		self->light = pb_type_ColorLight_external_obj_new(MP_OBJ_FROM_PTR(self), color_light_on);
	}
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
	idle(self);
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

static mp_obj_t ambient(mp_obj_t self) { return read_measurement(self, BP_SENSOR_AMBIENT, AMBIENT); }
static MP_DEFINE_CONST_FUN_OBJ_1(ambient_obj, ambient);
static mp_obj_t presence(mp_obj_t self) { return read_measurement(self, BP_SENSOR_PRESENCE, PRESENCE); }
static MP_DEFINE_CONST_FUN_OBJ_1(presence_obj, presence);
static mp_obj_t hsv(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, sensor_obj_t, self, PB_ARG_DEFAULT_TRUE(surface));
	return read_measurement(MP_OBJ_FROM_PTR(self), mp_obj_is_true(surface_in) ? BP_SENSOR_SURFACE : BP_SENSOR_AMBIENT, HSV);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(hsv_obj, 1, hsv);
static mp_obj_t color(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, sensor_obj_t, self, PB_ARG_DEFAULT_TRUE(surface));
	return read_measurement(MP_OBJ_FROM_PTR(self), mp_obj_is_true(surface_in) ? BP_SENSOR_SURFACE : BP_SENSOR_AMBIENT, COLOR);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(color_obj, 1, color);
static mp_obj_t surface_hsv(mp_obj_t self) { return read_measurement(self, BP_SENSOR_SURFACE, HSV); }
static MP_DEFINE_CONST_FUN_OBJ_1(surface_hsv_obj, surface_hsv);
static mp_obj_t surface_color(mp_obj_t self) { return read_measurement(self, BP_SENSOR_SURFACE, COLOR); }
static MP_DEFINE_CONST_FUN_OBJ_1(surface_color_obj, surface_color);

/* Adapted from util_pb/pb_color_map.c. The native port owns copied palettes. */
static mp_obj_t detectable_colors(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, sensor_obj_t, self, PB_ARG_DEFAULT_NONE(colors));
	struct bp_sensor_palette palette = {0};
	if (colors_in == mp_const_none) {
		check_connection(bp_sensor_palette_get(self->port, self->generation, &palette));
		mp_obj_t colors[BP_SENSOR_PALETTE_SIZE];
		for (uint32_t i = 0; i < palette.count; i++) colors[i] = pb_type_Color_from_hsv(palette.colors[i]);
		return mp_obj_new_tuple(palette.count, colors);
	}
	size_t count;
	mp_obj_t *colors;
	mp_obj_get_array(colors_in, &count, &colors);
	if (count > BP_SENSOR_PALETTE_SIZE) mp_raise_ValueError(MP_ERROR_TEXT("too many colors"));
	palette.count = count;
	for (uint32_t i = 0; i < count; i++) palette.colors[i] = pb_type_Color_get_hsv(colors[i]);
	check_connection(bp_sensor_palette_set(self->port, self->generation, &palette));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(detectable_colors_obj, 1, detectable_colors);

static mp_obj_t write_light(mp_obj_t parent, const struct bp_sensor_light *light)
{
	sensor_obj_t *self = MP_OBJ_TO_PTR(parent);
	idle(self);
	check_connection(bp_sensor_light_write(self->port, self->generation, light, &self->write_request));
	self->reading = WRITE;
	return wait_or_await(parent);
}

/* LightArray adapted from common/pb_type_lightarray.c; parsing is unchanged. */
typedef struct {
	mp_obj_base_t base;
	mp_obj_t parent;
} light_obj_t;

static mp_obj_t light_on(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, light_obj_t, self, PB_ARG_DEFAULT_INT(brightness, 100));
	sensor_obj_t *sensor = MP_OBJ_TO_PTR(self->parent);
	bool color = sensor->id == LEGO_DEVICE_TYPE_ID_SPIKE_COLOR_SENSOR;
	struct bp_sensor_light light = {.format = BP_SENSOR_LIGHT_BRIGHTNESS};
	pb_obj_get_pct_or_array(brightness_in, color ? 3 : 4, (int8_t *)light.brightness);
	return write_light(self->parent, &light);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(light_on_obj, 1, light_on);
static mp_obj_t light_off(mp_obj_t self_in)
{
	light_obj_t *self = MP_OBJ_TO_PTR(self_in);
	const struct bp_sensor_light light = {.format = BP_SENSOR_LIGHT_BRIGHTNESS};
	return write_light(self->parent, &light);
}
static MP_DEFINE_CONST_FUN_OBJ_1(light_off_obj, light_off);
static const mp_rom_map_elem_t light_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&light_on_obj)},
	{MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&light_off_obj)},
};
static MP_DEFINE_CONST_DICT(light_dict, light_methods);
static MP_DEFINE_CONST_OBJ_TYPE(light_type, MP_QSTR_LightArray, MP_TYPE_FLAG_NONE, locals_dict, &light_dict);
static mp_obj_t light_new(mp_obj_t parent)
{
	light_obj_t *self = mp_obj_malloc(light_obj_t, &light_type);
	self->parent = parent;
	return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t color_light_on(mp_obj_t parent, pbio_color_t color)
{
	const struct bp_sensor_light light = {.format = BP_SENSOR_LIGHT_HSV, .color = color};
	return write_light(parent, &light);
}

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
static const pb_attr_dict_entry_t lights_attributes[] = {
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_lights, sensor_obj_t, light),
	PB_ATTR_DICT_SENTINEL
};
static const pb_attr_dict_entry_t light_attributes[] = {
	PB_DEFINE_CONST_ATTR_RO(MP_QSTR_light, sensor_obj_t, light),
	PB_ATTR_DICT_SENTINEL
};
static const mp_rom_map_elem_t color_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_reflection), MP_ROM_PTR(&reflection_obj)},
	{MP_ROM_QSTR(MP_QSTR_ambient), MP_ROM_PTR(&ambient_obj)},
	{MP_ROM_QSTR(MP_QSTR_hsv), MP_ROM_PTR(&hsv_obj)},
	{MP_ROM_QSTR(MP_QSTR_color), MP_ROM_PTR(&color_obj)},
	{MP_ROM_QSTR(MP_QSTR_detectable_colors), MP_ROM_PTR(&detectable_colors_obj)},
};
static MP_DEFINE_CONST_DICT(color_dict, color_methods);
static MP_DEFINE_CONST_OBJ_TYPE(color_type, MP_QSTR_ColorSensor, MP_TYPE_FLAG_NONE,
	make_new, sensor_new, attr, pb_attribute_handler, protocol, lights_attributes, locals_dict, &color_dict);
static const mp_rom_map_elem_t ultrasonic_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_distance), MP_ROM_PTR(&distance_obj)},
	{MP_ROM_QSTR(MP_QSTR_presence), MP_ROM_PTR(&presence_obj)},
};
static MP_DEFINE_CONST_DICT(ultrasonic_dict, ultrasonic_methods);
static MP_DEFINE_CONST_OBJ_TYPE(ultrasonic_type, MP_QSTR_UltrasonicSensor, MP_TYPE_FLAG_NONE,
	make_new, sensor_new, attr, pb_attribute_handler, protocol, lights_attributes, locals_dict, &ultrasonic_dict);
static const mp_rom_map_elem_t color_distance_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_distance), MP_ROM_PTR(&distance_obj)},
	{MP_ROM_QSTR(MP_QSTR_reflection), MP_ROM_PTR(&reflection_obj)},
	{MP_ROM_QSTR(MP_QSTR_ambient), MP_ROM_PTR(&ambient_obj)},
	{MP_ROM_QSTR(MP_QSTR_hsv), MP_ROM_PTR(&surface_hsv_obj)},
	{MP_ROM_QSTR(MP_QSTR_color), MP_ROM_PTR(&surface_color_obj)},
	{MP_ROM_QSTR(MP_QSTR_detectable_colors), MP_ROM_PTR(&detectable_colors_obj)},
};
static MP_DEFINE_CONST_DICT(color_distance_dict, color_distance_methods);
static MP_DEFINE_CONST_OBJ_TYPE(color_distance_type, MP_QSTR_ColorDistanceSensor, MP_TYPE_FLAG_NONE,
	make_new, sensor_new, attr, pb_attribute_handler, protocol, light_attributes, locals_dict, &color_distance_dict);
static const mp_rom_map_elem_t pupdevices_globals[] = {
	{MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_pupdevices)},
	{MP_ROM_QSTR(MP_QSTR_ForceSensor), MP_ROM_PTR(&force_type)},
	{MP_ROM_QSTR(MP_QSTR_TiltSensor), MP_ROM_PTR(&tilt_type)},
	{MP_ROM_QSTR(MP_QSTR_InfraredSensor), MP_ROM_PTR(&infrared_type)},
	{MP_ROM_QSTR(MP_QSTR_ColorSensor), MP_ROM_PTR(&color_type)},
	{MP_ROM_QSTR(MP_QSTR_UltrasonicSensor), MP_ROM_PTR(&ultrasonic_type)},
	{MP_ROM_QSTR(MP_QSTR_ColorDistanceSensor), MP_ROM_PTR(&color_distance_type)},
};
static MP_DEFINE_CONST_DICT(pupdevices_dict, pupdevices_globals);
const mp_obj_module_t pb_module_pupdevices = {{&mp_type_module}, (mp_obj_dict_t *)&pupdevices_dict};
