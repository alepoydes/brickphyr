// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2025 The Pybricks Authors
/* Device modes, settling, force calibration and color adjustments selected
 * from PBIO port_lump/sensors.c and pybricks/pupdevices at a7b1b04ae. UART and
 * ownership remain outside these calculations. RGB/HSV conversion is reused
 * unchanged from PBIO. See docs/ports.md for provenance and units. */
#include "sensor.h"
#include <errno.h>
#include <lego/device.h>
#include <pbio/color.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* First entry for each device is its automatic measurement. Wire formats
 * are checked before SELECT; a recognized ID alone does not validate data. */
static const struct measurement {
	uint8_t id, measurement, mode, type, values, settle_ms;
} measurements[] = {
	{LEGO_DEVICE_TYPE_ID_SPIKE_COLOR_SENSOR, BP_SENSOR_SURFACE,
	 LEGO_DEVICE_MODE_PUP_COLOR_SENSOR__RGB_I, LUMP_DATA_TYPE_DATA16, 4, 30},
	{LEGO_DEVICE_TYPE_ID_SPIKE_COLOR_SENSOR, BP_SENSOR_AMBIENT,
	 LEGO_DEVICE_MODE_PUP_COLOR_SENSOR__SHSV, LUMP_DATA_TYPE_DATA16, 4, 30},
	{LEGO_DEVICE_TYPE_ID_SPIKE_ULTRASONIC_SENSOR, BP_SENSOR_DISTANCE,
	 LEGO_DEVICE_MODE_PUP_ULTRASONIC_SENSOR__DISTL, LUMP_DATA_TYPE_DATA16, 1, 50},
	{LEGO_DEVICE_TYPE_ID_SPIKE_ULTRASONIC_SENSOR, BP_SENSOR_PRESENCE,
	 LEGO_DEVICE_MODE_PUP_ULTRASONIC_SENSOR__LISTN, LUMP_DATA_TYPE_DATA8, 1, 50},
	{LEGO_DEVICE_TYPE_ID_SPIKE_FORCE_SENSOR, BP_SENSOR_FORCE,
	 LEGO_DEVICE_MODE_PUP_FORCE_SENSOR__CALIB, LUMP_DATA_TYPE_DATA16, 8, 0},
	{LEGO_DEVICE_TYPE_ID_COLOR_DIST_SENSOR, BP_SENSOR_SURFACE,
	 LEGO_DEVICE_MODE_PUP_COLOR_DISTANCE_SENSOR__RGB_I, LUMP_DATA_TYPE_DATA16, 3, 30},
	{LEGO_DEVICE_TYPE_ID_COLOR_DIST_SENSOR, BP_SENSOR_AMBIENT,
	 LEGO_DEVICE_MODE_PUP_COLOR_DISTANCE_SENSOR__AMBI, LUMP_DATA_TYPE_DATA8, 1, 30},
	{LEGO_DEVICE_TYPE_ID_COLOR_DIST_SENSOR, BP_SENSOR_DISTANCE,
	 LEGO_DEVICE_MODE_PUP_COLOR_DISTANCE_SENSOR__PROX, LUMP_DATA_TYPE_DATA8, 1, 30},
	{LEGO_DEVICE_TYPE_ID_WEDO2_TILT_SENSOR, BP_SENSOR_TILT,
	 LEGO_DEVICE_MODE_PUP_WEDO2_TILT_SENSOR__ANGLE, LUMP_DATA_TYPE_DATA8, 2, 0},
	{LEGO_DEVICE_TYPE_ID_WEDO2_MOTION_SENSOR, BP_SENSOR_DISTANCE,
	 LEGO_DEVICE_MODE_PUP_WEDO2_MOTION_SENSOR__CAL, LUMP_DATA_TYPE_DATA16, 3, 0},
	{LEGO_DEVICE_TYPE_ID_WEDO2_MOTION_SENSOR, BP_SENSOR_SURFACE,
	 LEGO_DEVICE_MODE_PUP_WEDO2_MOTION_SENSOR__CAL, LUMP_DATA_TYPE_DATA16, 3, 0},
	{LEGO_DEVICE_TYPE_ID_WEDO2_MOTION_SENSOR, BP_SENSOR_COUNT,
	 LEGO_DEVICE_MODE_PUP_WEDO2_MOTION_SENSOR__COUNT, LUMP_DATA_TYPE_DATA32, 1, 0},
	{LEGO_DEVICE_TYPE_ID_INTERACTIVE_MOTOR, BP_SENSOR_ANGLE,
	 LEGO_DEVICE_MODE_PUP_REL_MOTOR__POS, LUMP_DATA_TYPE_DATA32, 1, 0},
	{LEGO_DEVICE_TYPE_ID_TECHNIC_L_MOTOR, BP_SENSOR_ANGLE,
	 LEGO_DEVICE_MODE_PUP_ABS_MOTOR__CALIB, LUMP_DATA_TYPE_DATA16, 2, 0},
	{LEGO_DEVICE_TYPE_ID_TECHNIC_XL_MOTOR, BP_SENSOR_ANGLE,
	 LEGO_DEVICE_MODE_PUP_ABS_MOTOR__CALIB, LUMP_DATA_TYPE_DATA16, 2, 0},
	{LEGO_DEVICE_TYPE_ID_SPIKE_M_MOTOR, BP_SENSOR_ANGLE,
	 LEGO_DEVICE_MODE_PUP_ABS_MOTOR__CALIB, LUMP_DATA_TYPE_DATA16, 2, 0},
	{LEGO_DEVICE_TYPE_ID_SPIKE_L_MOTOR, BP_SENSOR_ANGLE,
	 LEGO_DEVICE_MODE_PUP_ABS_MOTOR__CALIB, LUMP_DATA_TYPE_DATA16, 2, 0},
	{LEGO_DEVICE_TYPE_ID_SPIKE_S_MOTOR, BP_SENSOR_ANGLE,
	 LEGO_DEVICE_MODE_PUP_ABS_MOTOR__CALIB, LUMP_DATA_TYPE_DATA16, 2, 0},
	{LEGO_DEVICE_TYPE_ID_TECHNIC_M_ANGULAR_MOTOR, BP_SENSOR_ANGLE,
	 LEGO_DEVICE_MODE_PUP_ABS_MOTOR__CALIB, LUMP_DATA_TYPE_DATA16, 2, 0},
	{LEGO_DEVICE_TYPE_ID_TECHNIC_L_ANGULAR_MOTOR, BP_SENSOR_ANGLE,
	 LEGO_DEVICE_MODE_PUP_ABS_MOTOR__CALIB, LUMP_DATA_TYPE_DATA16, 2, 0},
};

static const struct measurement *find(uint16_t id, uint32_t measurement)
{
	for (size_t i = 0; i < ARRAY_SIZE(measurements); i++) {
		if (measurements[i].id == id && (!measurement || measurements[i].measurement == measurement)) {
			return &measurements[i];
		}
	}
	return NULL;
}

static bool valid(const struct measurement *m, const struct lump *l)
{
	const struct bp_port_mode *raw = &l->modes[LEGO_DEVICE_MODE_PUP_FORCE_SENSOR__FRAW];
	return m && m->mode < l->num_modes && l->modes[m->mode].data_type == m->type &&
		l->modes[m->mode].num_values == m->values &&
		(m->measurement != BP_SENSOR_ANGLE || m->id == LEGO_DEVICE_TYPE_ID_INTERACTIVE_MOTOR ||
		 (l->capabilities & LUMP_MODE_FLAGS0_MOTOR_ABS_POS)) &&
		(m->id != LEGO_DEVICE_TYPE_ID_SPIKE_FORCE_SENSOR ||
		 (raw->data_type == LUMP_DATA_TYPE_DATA16 && raw->num_values == 1));
}

void port_sensor_reset(struct port_sensor *s, const struct lump *l)
{
	*s = (struct port_sensor){.value = {.type_id = l->id, .error = -ENOTSUP}, .mode = UINT8_MAX};
	for (uint32_t i = BP_SENSOR_DEFAULT; i < BP_SENSOR_MEASUREMENT_COUNT; i++) {
		if (valid(find(l->id, i), l)) {
			s->value.available |= BIT(i);
		}
	}
}

static void select_mode(struct port_sensor *s, uint8_t mode)
{
	s->mode = mode;
	s->pending = true;
	s->awaiting = false;
	s->first_ms = -1;
	s->value.fields = 0;
	s->value.error = -EAGAIN;
}

int port_sensor_request(struct port_sensor *s, const struct lump *l, uint32_t measurement)
{
	const struct measurement *m = find(l->id, measurement);
	if (!m) {
		return -ENOTSUP;
	}
	if (!valid(m, l)) {
		return -EPROTO;
	}
	/* valid() checks both force formats before even selecting calibration. */
	uint8_t mode = m->id == LEGO_DEVICE_TYPE_ID_SPIKE_FORCE_SENSOR && s->calibrated ?
		LEGO_DEVICE_MODE_PUP_FORCE_SENSOR__FRAW : m->mode;
	s->value.measurement = m->measurement;
	if (s->mode != mode) {
		select_mode(s, mode);
	}
	return 0;
}

void port_sensor_sent(struct port_sensor *s, int64_t now)
{
	s->pending = false;
	s->awaiting = true;
	s->first_ms = -1;
	s->deadline = now + 500;
}

static int convert(struct port_sensor *s, const struct lump *l, struct bp_sensor_sample *v)
{
	int16_t a = l->length >= 2 ? (int16_t)sys_get_le16(l->data) : (int8_t)l->data[0];
	int16_t b = l->length >= 4 ? (int16_t)sys_get_le16(l->data + 2) : 0;
	int16_t c = l->length >= 6 ? (int16_t)sys_get_le16(l->data + 4) : 0;
	if (v->measurement == BP_SENSOR_ANGLE) {
		v->fields = BP_SENSOR_HAS_ANGLE;
		int32_t current;
		int64_t delta;
		if (l->id == LEGO_DEVICE_TYPE_ID_INTERACTIVE_MOTOR) {
			/* Extend the device's signed degree counter through its 32-bit
			 * rollover. Unsigned subtraction avoids signed overflow. */
			current = (int32_t)sys_get_le32(l->data);
			delta = (int64_t)(int32_t)((uint32_t)current - (uint32_t)s->previous_angle) * 1000;
			v->angle_mdeg = (int64_t)current * 1000;
		} else {
			/* PBIO reads the second CALIB word in decidegrees. Accept its
			 * closed 0..3600 range, with both endpoints meaning zero. */
			if (b < 0 || b > 3600) {
				return -EBADMSG;
			}
			current = b >= 1800 ? (b - 3600) * 100 : b * 100;
			delta = current - s->previous_angle;
			if (delta >= 180000) delta -= 360000;
			if (delta < -180000) delta += 360000;
			v->angle_mdeg = current;
			v->absolute_mdeg = current;
			v->fields |= BP_SENSOR_HAS_ABSOLUTE_ANGLE;
		}
		if (s->angle_initialized && __builtin_add_overflow(s->value.angle_mdeg, delta, &v->angle_mdeg)) {
			return -ERANGE;
		}
		s->previous_angle = current;
		s->angle_initialized = true;
		return 0;
	}
	switch (l->id) {
	case LEGO_DEVICE_TYPE_ID_SPIKE_FORCE_SENSOR:
		if (l->mode == LEGO_DEVICE_MODE_PUP_FORCE_SENSOR__CALIB) {
			int16_t end = (int16_t)sys_get_le16(l->data + 12);
			if (c >= end) {
				return -EBADMSG;
			}
			s->offset = b;
			s->released = c;
			s->end = end;
			s->calibrated = true;
			select_mode(s, LEGO_DEVICE_MODE_PUP_FORCE_SENSOR__FRAW);
			return -EAGAIN;
		}
		v->fields = BP_SENSOR_HAS_FORCE;
		v->travel_um = 6670 * ((int32_t)a - s->released) / (s->end - s->released);
		v->force_mn = MAX(0, 10000 * ((int32_t)a - s->released - s->offset) / (s->end - s->released));
		return 0;
	case LEGO_DEVICE_TYPE_ID_SPIKE_ULTRASONIC_SENSOR:
		if (v->measurement == BP_SENSOR_DISTANCE) {
			v->fields = BP_SENSOR_HAS_DISTANCE;
			v->distance_mm = a < 0 || a >= 2000 ? 2000 : a;
		} else {
			v->fields = BP_SENSOR_HAS_PRESENCE;
			v->presence = l->data[0] != 0;
		}
		return 0;
	case LEGO_DEVICE_TYPE_ID_WEDO2_TILT_SENSOR:
		v->fields = BP_SENSOR_HAS_TILT;
		v->tilt_deg[0] = (int8_t)l->data[1];
		v->tilt_deg[1] = (int8_t)l->data[0];
		return 0;
	case LEGO_DEVICE_TYPE_ID_WEDO2_MOTION_SENSOR:
		if (v->measurement == BP_SENSOR_COUNT) {
			v->fields = BP_SENSOR_HAS_COUNT;
			v->count = (int32_t)sys_get_le32(l->data);
		} else {
			if (a < 0) {
				return -EBADMSG;
			}
			v->fields = BP_SENSOR_HAS_RELATIVE_DISTANCE | BP_SENSOR_HAS_REFLECTION;
			v->distance_pct = 1100 / (10 + a);
			v->reflection_permille = a * 2;
		}
		return 0;
	case LEGO_DEVICE_TYPE_ID_COLOR_DIST_SENSOR:
		if (v->measurement == BP_SENSOR_DISTANCE || v->measurement == BP_SENSOR_AMBIENT) {
			if (a < 0) {
				return -EBADMSG;
			}
			if (v->measurement == BP_SENSOR_DISTANCE) {
				v->fields = BP_SENSOR_HAS_RELATIVE_DISTANCE;
				v->distance_pct = a * 10;
			} else {
				v->fields = BP_SENSOR_HAS_AMBIENT;
				v->ambient_permille = a * 10;
			}
			return 0;
		}
		break;
	case LEGO_DEVICE_TYPE_ID_SPIKE_COLOR_SENSOR:
		if (v->measurement == BP_SENSOR_AMBIENT) {
			if (a < 0 || a >= 360 || b < 0 || c < 0) {
				return -EBADMSG;
			}
			v->fields = BP_SENSOR_HAS_AMBIENT | BP_SENSOR_HAS_HSV;
			v->hue_deg = a;
			v->saturation_pct = MIN(b / 10, 100);
			v->value_pct = MIN(c / 10, 100);
			v->ambient_permille = c / 10;
			return 0;
		}
		break;
	default:
		return -ENOTSUP;
	}
	if (a < 0 || b < 0 || c < 0) {
		return -EBADMSG;
	}
	bool spike = l->id == LEGO_DEVICE_TYPE_ID_SPIKE_COLOR_SENSOR;
	if (spike && (a > 1024 || b > 1024 || c > 1024)) {
		return -EBADMSG;
	}
	/* Bound before narrowing; unusually bright BOOST input must saturate,
	 * not wrap the RGB channel as a uint8_t cast would. */
	pbio_color_rgb_t rgb = {
		.r = spike ? MIN(a / 4, 255) : MIN(1187 * a / 2048, 255),
		.g = spike ? MIN(b / 4, 255) : MIN(1187 * b / 2048, 255),
		.b = spike ? MIN(c / 4, 255) : MIN(1187 * c / 2048, 255),
	};
	pbio_color_t color = pbio_color_from_rgb_with_hue_shift(&rgb);
	int sat = pbio_color_get_s(color), val = pbio_color_get_v(color);
	v->fields = BP_SENSOR_HAS_HSV | BP_SENSOR_HAS_REFLECTION;
	v->hue_deg = pbio_color_get_h(color);
	v->saturation_pct = spike ? sat * (200 - sat) / 100 : sat;
	v->value_pct = spike ? val * (150 - val / 2) / 100 : val * (200 - val) / 100;
	v->reflection_permille = spike ? (a + b + c) * 1000 / 3072 : MIN((a + b + c) * 10 / 12, 1000);
	return 0;
}

void port_sensor_receive(struct port_sensor *s, const struct lump *l, int64_t now)
{
	const struct measurement *m = find(l->id, s->value.measurement);
	if (s->pending || s->mode != l->mode || !m) {
		return;
	}
	/* Other modes cannot keep a stopped measurement alive. Use the UART
	 * service's three-keepalive bound after the first matching frame. */
	s->deadline = now + 300;
	if (s->first_ms < 0) {
		s->first_ms = now;
		s->awaiting = false;
	}
	if (now - s->first_ms < m->settle_ms) {
		return;
	}
	struct bp_sensor_sample value = {.type_id = l->id, .mode = l->mode,
		.measurement = s->value.measurement, .available = s->value.available,
		.sequence = s->value.sequence + 1, .sample_ms = now};
	int err = convert(s, l, &value);
	if (err) {
		s->value.error = err;
		s->value.fields = 0;
	} else {
		s->value = value;
	}
}
