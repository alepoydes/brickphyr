// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <brickphyr_sensor.h>

#define BP_LWP3_NAME_MAX 14
#define BP_LWP3_MESSAGE_MAX 20
#define BP_LWP3_PAIR 1

enum bp_lwp3_profile { BP_LWP3_RAW, BP_LWP3_REMOTE, BP_LWP3_TECHNIC_MOVE, BP_LWP3_MARIO, BP_LWP3_DUPLO };
enum bp_lwp3_command { BP_LWP3_LIGHT, BP_LWP3_DRIVE, BP_LWP3_SOUND };
enum bp_lwp3_sound { BP_LWP3_BRAKE = 1, BP_LWP3_DEPART, BP_LWP3_WATER, BP_LWP3_HORN, BP_LWP3_STEAM };
enum bp_lwp3_fields { BP_LWP3_HAS_RGB = 1, BP_LWP3_HAS_TAG = 2, BP_LWP3_HAS_SPEED = 4,
	BP_LWP3_HAS_MATCHED_COLOR = 8 };

struct bp_lwp3_config {
	uint8_t kind, notifications, flags;
	/* RAW uses kind/flags. Named profiles select their supported hub kinds,
	 * pairing and post-connect setup; kind/flags must then be zero. */
	uint8_t profile;
};

struct bp_lwp3_state {
	uint8_t kind;
	/* Remote: left +, center, -; right +, center, -; green center. */
	uint8_t buttons;
	/* Fields describe accepted notifications on this link. Receipt times are
	 * monotonic ms; a new claim/connect invalidates previous sensor values. */
	uint8_t fields;
	int64_t color_ms, speed_ms;
	int16_t speed; /* Signed train speed in the reference protocol's units. */
	uint16_t color_raw[3]; /* RGB channels, or last train tag in element 0. */
	/* Packed HSV: hsv requires HAS_RGB; matched_color requires HAS_MATCHED_COLOR. */
	uint32_t hsv, matched_color;
};

struct bp_lwp3_message {
	uint8_t size;
	uint8_t data[BP_LWP3_MESSAGE_MAX];
};
