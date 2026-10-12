// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <brickphyr_port.h>

enum bp_sensor_measurement {
	BP_SENSOR_DEFAULT, BP_SENSOR_SURFACE, BP_SENSOR_AMBIENT,
	BP_SENSOR_DISTANCE, BP_SENSOR_PRESENCE, BP_SENSOR_FORCE,
	BP_SENSOR_TILT, BP_SENSOR_COUNT, BP_SENSOR_ANGLE,
	BP_SENSOR_MEASUREMENT_COUNT,
};
enum bp_sensor_fields {
	BP_SENSOR_HAS_DISTANCE = 1 << 0, /* millimetres, ultrasonic only */
	BP_SENSOR_HAS_RELATIVE_DISTANCE = 1 << 1, /* percent, infrared */
	BP_SENSOR_HAS_REFLECTION = 1 << 2,
	BP_SENSOR_HAS_AMBIENT = 1 << 3,
	BP_SENSOR_HAS_HSV = 1 << 4,
	BP_SENSOR_HAS_FORCE = 1 << 5,
	BP_SENSOR_HAS_TILT = 1 << 6,
	BP_SENSOR_HAS_COUNT = 1 << 7,
	BP_SENSOR_HAS_PRESENCE = 1 << 8,
	BP_SENSOR_HAS_ANGLE = 1 << 9,
	BP_SENSOR_HAS_ABSOLUTE_ANGLE = 1 << 10,
	BP_SENSOR_HAS_MATCHED_COLOR = 1 << 11,
};
#define BP_SENSOR_PALETTE_SIZE 8
/* Packed HSV: 0xHHHHSSVV, hue 0..359, saturation 0..100, signed value -100..100.
 * A zero-length palette disables matching. Color entries are copied. */
struct bp_sensor_palette {
	uint32_t count;
	uint32_t colors[BP_SENSOR_PALETTE_SIZE];
};
enum bp_sensor_light_format { BP_SENSOR_LIGHT_BRIGHTNESS, BP_SENSOR_LIGHT_HSV };
struct bp_sensor_light {
	uint32_t format;
	union {
		uint8_t brightness[4]; /* 0..100; SPIKE color uses first 3, ultrasonic all 4 */
		uint32_t color; /* packed HSV; BOOST light chooses red/green/blue/off */
	};
};
struct bp_sensor_sample {
	/* Parser completion time of the last accepted measurement, monotonic ms.
	 * Check error and fields before using values. Reads never select a mode. */
	int64_t sample_ms;
	uint32_t generation, sequence, fields;
	uint32_t available; /* bit (1 << bp_sensor_measurement), including DEFAULT */
	int32_t error;
	uint16_t type_id;
	uint8_t mode, measurement, owner;
	int32_t distance_mm; /* 2000 means no ultrasonic return below 2 m */
	int32_t distance_pct; /* relative infrared estimate, not a length */
	int32_t reflection_permille, ambient_permille;
	int32_t force_mn, travel_um;
	int32_t tilt_deg[2]; /* Pybricks tilt order */
	int32_t count; /* sensor counter, no application-specific zero offset */
	/* Encoder direction is the device's, without gear or zero adjustments.
	 * Absolute encoders require <180 degrees between accepted samples to
	 * count turns; connection generation changes restart that count. */
	int64_t angle_mdeg;
	int32_t absolute_mdeg; /* [-180000, 180000), when HAS_ABSOLUTE_ANGLE */
	uint16_t hue_deg;
	uint8_t saturation_pct, value_pct, presence;
	uint32_t matched_color; /* packed HSV; requires HAS_MATCHED_COLOR */
};
