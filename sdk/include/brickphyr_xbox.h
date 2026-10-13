// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

enum bp_xbox_button {
	BP_XBOX_A, BP_XBOX_B, BP_XBOX_X, BP_XBOX_Y, BP_XBOX_LB, BP_XBOX_RB,
	BP_XBOX_VIEW, BP_XBOX_MENU, BP_XBOX_GUIDE, BP_XBOX_LJ, BP_XBOX_RJ,
	BP_XBOX_UPLOAD, BP_XBOX_P1, BP_XBOX_P2, BP_XBOX_P3, BP_XBOX_P4,
	BP_XBOX_UP, BP_XBOX_RIGHT, BP_XBOX_DOWN, BP_XBOX_LEFT,
};
struct bp_xbox_state {
	/* A complete input report is required; no invented neutral state. */
	int32_t error;
	int64_t sample_ms;
	uint32_t buttons;
	/* Raw HID values and normalized percentages. X right/Y up are positive. */
	uint16_t axes[4], triggers[2];
	uint16_t raw_buttons;
	uint8_t upload;
	int16_t joystick_pct[4];
	uint8_t trigger_pct[2], dpad, profile, paddles, trigger_switches;
	uint8_t valid, has_profile;
};
struct bp_xbox_rumble {
	/* Percent: left/right main actuator, left/right trigger actuator. */
	uint8_t power[4];
	uint16_t duration_ms, delay_ms;
	uint8_t count;
};
