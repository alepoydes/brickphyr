// SPDX-License-Identifier: Zlib AND Apache-2.0
/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
/* Adapted Xbox BLE 5.x report layout from SDL release 3.2.28,
 * src/joystick/hidapi/SDL_hidapi_xboxone.c (7f3ae3d57459e59943a4ecfefc8f6277ec6bf540).
 * Rumble report fields follow Bluepad32 parser/uni_hid_parser_xboxone.c
 * (80d6314e46f871719fbb4b721a2cc02227c4f369), Copyright 2019
 * Ricardo Quesada, Apache-2.0. This is a Brickphyr-specific codec;
 * neither upstream transport nor either upstream source file is compiled. */
#include "xbox_device.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>

static uint16_t get_le16(const uint8_t *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int16_t axis_percent(uint16_t raw, bool invert)
{
	int32_t delta = (int32_t)raw - 32768;
	if (invert) delta = -delta;
	int32_t magnitude = delta < 0 ? -delta : delta;
	int32_t range = delta < 0 ? 32768 : 32767;
	int32_t percent = (magnitude * 100 + range / 2) / range;
	return delta < 0 ? -percent : percent;
}

static void apply_deadzone(int16_t *x, int16_t *y, uint8_t deadzone)
{
	if (*x < deadzone && *x > -(int)deadzone &&
	    *y < deadzone && *y > -(int)deadzone) {
		*x = 0;
		*y = 0;
	}
}

int xbox_device_input(struct bp_xbox_state *state, const uint8_t *data,
	size_t size, uint8_t deadzone, int64_t now)
{
	if (!state || !data || (size != 16 && size != 19) || deadzone > 100 ||
	    data[12] > 8 || get_le16(data + 8) > 1023 ||
	    get_le16(data + 10) > 1023) return -EINVAL;

	struct bp_xbox_state next = *state;
	for (size_t i = 0; i < 4; i++) {
		next.axes[i] = get_le16(data + i * 2);
		next.joystick_pct[i] = axis_percent(next.axes[i], i == 1 || i == 3);
	}
	apply_deadzone(&next.joystick_pct[0], &next.joystick_pct[1], deadzone);
	apply_deadzone(&next.joystick_pct[2], &next.joystick_pct[3], deadzone);
	for (size_t i = 0; i < 2; i++) {
		next.triggers[i] = get_le16(data + 8 + i * 2);
		next.trigger_pct[i] = (next.triggers[i] * 100 + 511) / 1023;
	}
	next.dpad = data[12];
	next.raw_buttons = get_le16(data + 13);
	next.upload = data[15];
	next.buttons = 0;
	static const uint8_t first_buttons[] = {
		BP_XBOX_A, BP_XBOX_B, 0xff, BP_XBOX_X,
		BP_XBOX_Y, 0xff, BP_XBOX_LB, BP_XBOX_RB,
	};
	static const uint8_t second_buttons[] = {
		0xff, 0xff, BP_XBOX_VIEW, BP_XBOX_MENU,
		BP_XBOX_GUIDE, BP_XBOX_LJ, BP_XBOX_RJ, 0xff,
	};
	for (size_t i = 0; i < 8; i++) {
		if ((data[13] & (1U << i)) && first_buttons[i] != 0xff) {
			next.buttons |= 1U << first_buttons[i];
		}
		if ((data[14] & (1U << i)) && second_buttons[i] != 0xff) {
			next.buttons |= 1U << second_buttons[i];
		}
	}
	if (data[15] & 1) next.buttons |= 1U << BP_XBOX_UPLOAD;
	static const uint32_t hat_buttons[9] = {
		0, 1U << BP_XBOX_UP, (1U << BP_XBOX_UP) | (1U << BP_XBOX_RIGHT),
		1U << BP_XBOX_RIGHT, (1U << BP_XBOX_RIGHT) | (1U << BP_XBOX_DOWN),
		1U << BP_XBOX_DOWN, (1U << BP_XBOX_DOWN) | (1U << BP_XBOX_LEFT),
		1U << BP_XBOX_LEFT, (1U << BP_XBOX_LEFT) | (1U << BP_XBOX_UP),
	};
	next.buttons |= hat_buttons[next.dpad];
	next.has_profile = size == 19;
	next.profile = next.has_profile ? data[16] : 0;
	next.trigger_switches = next.has_profile ? data[17] : 0;
	next.paddles = next.has_profile ? data[18] & 0x0f : 0;
	for (size_t i = 0; i < 4; i++) {
		if (next.paddles & (1U << i)) next.buttons |= 1U << (BP_XBOX_P1 + i);
	}
	next.sample_ms = now;
	next.valid = 1;
	next.error = 0;
	*state = next;
	return 0;
}

int xbox_device_rumble(const struct bp_xbox_rumble *rumble, uint8_t data[8])
{
	if (!rumble || !data || rumble->duration_ms > 2500 ||
	    rumble->delay_ms > 2500 || rumble->count > 100) return -EINVAL;
	for (size_t i = 0; i < 4; i++) {
		if (rumble->power[i] > 100) return -EINVAL;
	}
	if (!rumble->count || !rumble->duration_ms ||
	    !(rumble->power[0] | rumble->power[1] | rumble->power[2] | rumble->power[3])) return 0;
	const uint8_t output[8] = {
		0x0f, rumble->power[2], rumble->power[3],
		rumble->power[0], rumble->power[1],
		(uint8_t)(rumble->duration_ms < 10 ? 1 : rumble->duration_ms / 10),
		(uint8_t)(rumble->count > 1 ? rumble->delay_ms / 10 : 0),
		(uint8_t)(rumble->count - 1),
	};
	memcpy(data, output, sizeof(output));
	return sizeof(output);
}
