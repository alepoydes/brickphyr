// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2024 The Pybricks Authors
/* LWP3 profile packets and color handling selected from
 * pybricks/iodevices/pb_type_iodevices_lwp3device.c at a7b1b04ae.
 * Connection ownership and ATT remain with Brickphyr's radio worker. */
#include "lwp3_devices.h"
#include <errno.h>
#include <string.h>
#include <lego/lwp3.h>
#include <pbio/color.h>
#include <pbio/color_map.h>
#include <pbio/int_math.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

bool lwp3_device_matches(const struct bp_lwp3_config *config, uint8_t kind)
{
	switch (config->profile) {
	case BP_LWP3_RAW: return kind == config->kind;
	case BP_LWP3_REMOTE: return kind == LWP3_HUB_KIND_HANDSET;
	case BP_LWP3_TECHNIC_MOVE: return kind == LWP3_HUB_KIND_TECHNIC_MOVE;
	case BP_LWP3_MARIO:
		return kind == LWP3_HUB_KIND_MARIO || kind == LWP3_HUB_KIND_LUIGI ||
			kind == LWP3_HUB_KIND_PEACH;
	case BP_LWP3_DUPLO:
		return kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLACK ||
			kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLUE;
	default: return false;
	}
}

bool lwp3_device_pair(const struct bp_lwp3_config *config, uint8_t kind)
{
	if (!lwp3_device_matches(config, kind)) return false;
	switch (config->profile) {
	case BP_LWP3_RAW: return config->flags & BP_LWP3_PAIR;
	case BP_LWP3_TECHNIC_MOVE:
	case BP_LWP3_MARIO: return true;
	case BP_LWP3_DUPLO: return kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLUE;
	default: return false;
	}
}

static int copy_packet(uint8_t data[BP_LWP3_MESSAGE_MAX], const uint8_t *packet, size_t size)
{
	memcpy(data, packet, size);
	return size;
}

static int mode_setup(uint8_t port, uint8_t mode, uint8_t notify,
	uint8_t data[BP_LWP3_MESSAGE_MAX])
{
	const uint8_t packet[] = {10, 0, LWP3_MSG_TYPE_PORT_MODE_SETUP,
		port, mode, 1, 0, 0, 0, notify};
	return copy_packet(data, packet, sizeof(packet));
}

static int remote_light(pbio_color_t hsv, uint8_t data[BP_LWP3_MESSAGE_MAX])
{
	pbio_color_rgb_t rgb;
	pbio_color_to_rgb(hsv, &rgb);
	const uint8_t packet[] = {10, 0, LWP3_MSG_TYPE_OUT_PORT_CMD, 52, 0,
		LWP3_OUTPUT_CMD_WRITE_DIRECT_MODE_DATA, 1,
		rgb.r, rgb.g * 3 / 8, rgb.b * 3 / 8};
	return copy_packet(data, packet, sizeof(packet));
}

int lwp3_device_setup(uint8_t profile, uint8_t kind, uint8_t step,
	uint8_t data[BP_LWP3_MESSAGE_MAX])
{
	const struct bp_lwp3_config config = {.profile = profile, .kind = kind};
	if (!lwp3_device_matches(&config, kind)) return -EINVAL;
	switch (profile) {
	case BP_LWP3_RAW: return 0;
	case BP_LWP3_REMOTE:
		if (step < 3) return mode_setup(step == 2 ? 52 : step,
			step == 2 ? 1 : 4, step == 2 ? 0 : 1, data);
		return step == 3 ? remote_light(PBIO_COLOR_BLUE, data) : 0;
	case BP_LWP3_TECHNIC_MOVE: {
		if (step > 1) return 0;
		const uint8_t packet[] = {13, 0, LWP3_MSG_TYPE_OUT_PORT_CMD, 0x36,
			0x11, LWP3_OUTPUT_CMD_WRITE_DIRECT_MODE_DATA, 0, 3, 0, 0, 0,
			step ? 0x08 : 0x10, 0};
		return copy_packet(data, packet, sizeof(packet));
	}
	case BP_LWP3_MARIO:
		return step ? 0 : mode_setup(1, 1, 1, data);
	case BP_LWP3_DUPLO:
		if (step > 2) return 0;
		if (!step) return mode_setup(1, 1, 1, data);
		if (kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLACK) {
			return step == 1 ? mode_setup(0x12, 3, 1, data) :
				mode_setup(0x13, 0, 1, data);
		}
		return step == 1 ? mode_setup(0x33, 0, 1, data) :
			mode_setup(0x36, 2, 1, data);
	default: return -EINVAL;
	}
}

static bool valid_hsv(pbio_color_t hsv)
{
	return pbio_color_get_h(hsv) < 360 && pbio_color_get_s(hsv) <= 100 &&
		pbio_color_get_v(hsv) >= -100 && pbio_color_get_v(hsv) <= 100;
}

static uint8_t duplo_light_id(uint8_t kind, pbio_color_t hsv)
{
	uint16_t h = pbio_color_get_h(hsv);
	uint8_t s = pbio_color_get_s(hsv);
	int8_t v = pbio_color_get_v(hsv);
	if (kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLACK) {
		if (s < 10) return v > 50 ? 10 : 0;
		if (h < 15) return 9;
		if (h < 45) return 8;
		if (h < 90) return 7;
		if (h < 150) return 6;
		if (h < 170) return 5;
		if (h < 210) return 4;
		if (h < 270) return 3;
		return h < 305 ? 2 : 1;
	}
	if (s < 10) return v > 50 ? 1 : 0;
	if (h < 15) return 2;
	if (h < 90) return 4;
	if (h < 150) return 5;
	if (h < 210) return 3;
	return h < 270 ? 15 : 14;
}

int lwp3_device_command(uint8_t profile, uint8_t kind, uint32_t command,
	int32_t value, int32_t auxiliary, uint8_t data[BP_LWP3_MESSAGE_MAX])
{
	const struct bp_lwp3_config config = {.profile = profile, .kind = kind};
	if (!lwp3_device_matches(&config, kind)) return -EINVAL;
	if (command == BP_LWP3_LIGHT) {
		if (profile != BP_LWP3_REMOTE && profile != BP_LWP3_DUPLO) return -ENOTSUP;
		if (auxiliary || !valid_hsv((pbio_color_t)value)) return -EINVAL;
		if (profile == BP_LWP3_REMOTE) return remote_light((pbio_color_t)value, data);
		uint8_t id = duplo_light_id(kind, (pbio_color_t)value);
		if (kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLACK) {
			const uint8_t packet[] = {8, 0, LWP3_MSG_TYPE_OUT_PORT_CMD, 0x11,
				0x11, LWP3_OUTPUT_CMD_WRITE_DIRECT_MODE_DATA, 0, id};
			return copy_packet(data, packet, sizeof(packet));
		}
		const uint8_t packet[] = {10, 0, LWP3_MSG_TYPE_OUT_PORT_CMD, 52,
			0x11, LWP3_OUTPUT_CMD_WRITE_DIRECT_MODE_DATA, 1, 4, 1, id};
		return copy_packet(data, packet, sizeof(packet));
	}
	if (command == BP_LWP3_DRIVE) {
		if (profile == BP_LWP3_TECHNIC_MOVE) {
			const uint8_t packet[] = {13, 0, LWP3_MSG_TYPE_OUT_PORT_CMD, 0x36,
				0x11, LWP3_OUTPUT_CMD_WRITE_DIRECT_MODE_DATA, 0, 3, 0,
				(uint8_t)pbio_int_math_clamp(value, 100),
				(uint8_t)pbio_int_math_clamp(auxiliary, 97), 0, 0};
			return copy_packet(data, packet, sizeof(packet));
		}
		if (profile != BP_LWP3_DUPLO) return -ENOTSUP;
		if (auxiliary) return -EINVAL;
		const uint8_t packet[] = {8, 0, LWP3_MSG_TYPE_OUT_PORT_CMD,
			kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLACK ? 0 : 0x32,
			1, LWP3_OUTPUT_CMD_WRITE_DIRECT_MODE_DATA, 0,
			(uint8_t)pbio_int_math_clamp(value, 100)};
		return copy_packet(data, packet, sizeof(packet));
	}
	if (command == BP_LWP3_SOUND) {
		if (profile != BP_LWP3_DUPLO) return -ENOTSUP;
		if (auxiliary || value < BP_LWP3_BRAKE || value > BP_LWP3_STEAM) return -EINVAL;
		if (kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLUE) {
			/* The reference has one known second-generation sound packet. */
			const uint8_t packet[] = {9, 0, LWP3_MSG_TYPE_OUT_PORT_CMD, 52,
				0x11, LWP3_OUTPUT_CMD_WRITE_DIRECT_MODE_DATA, 1, 7, 1};
			return copy_packet(data, packet, sizeof(packet));
		}
		static const uint8_t sounds[] = {3, 5, 7, 9, 10};
		const uint8_t packet[] = {8, 0, LWP3_MSG_TYPE_OUT_PORT_CMD, 1,
			0x11, LWP3_OUTPUT_CMD_WRITE_DIRECT_MODE_DATA, 1,
			sounds[value - BP_LWP3_BRAKE]};
		return copy_packet(data, packet, sizeof(packet));
	}
	return -ENOTSUP;
}

void lwp3_device_notify(struct bp_lwp3_state *state, const uint8_t *data,
	size_t size, int64_t now)
{
	if (!data || size < 5 || size > BP_LWP3_MESSAGE_MAX || data[0] != size || data[1]) return;
	if (state->kind == LWP3_HUB_KIND_HANDSET) {
		if (size == 5 && data[2] == LWP3_MSG_TYPE_HW_NET_CMDS &&
		    data[3] == LWP3_HW_NET_CMD_CONNECTION_REQ) {
			state->buttons = (state->buttons & 0x3f) | (data[4] ? 0x40 : 0);
		} else if (size == 7 && data[2] == LWP3_MSG_TYPE_PORT_VALUE && data[3] < 2) {
			uint8_t shift = data[3] * 3;
			uint8_t buttons = (!!data[4]) | (!!data[5] << 1) | (!!data[6] << 2);
			state->buttons = (state->buttons & ~(7 << shift)) | (buttons << shift);
		}
		return;
	}
	if (data[2] != LWP3_MSG_TYPE_PORT_VALUE) return;
	if (state->kind == LWP3_HUB_KIND_MARIO || state->kind == LWP3_HUB_KIND_LUIGI ||
	    state->kind == LWP3_HUB_KIND_PEACH) {
		if (size != 7 || data[3] != 1) return;
		for (size_t i = 0; i < 3; i++) state->color_raw[i] = data[4 + i];
		state->fields = (state->fields & ~BP_LWP3_HAS_TAG) | BP_LWP3_HAS_RGB;
		state->color_ms = now;
		return;
	}
	if (state->kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLACK) {
		if (size == 6 && data[3] == 0x13) {
			state->speed = (int16_t)sys_get_le16(data + 4);
			state->fields |= BP_LWP3_HAS_SPEED;
			state->speed_ms = now;
		} else if (size == 10 && data[3] == 0x12) {
			for (size_t i = 0; i < 3; i++) state->color_raw[i] = sys_get_le16(data + 4 + i * 2);
			state->fields = (state->fields & ~BP_LWP3_HAS_TAG) | BP_LWP3_HAS_RGB;
			state->color_ms = now;
		}
	} else if (state->kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLUE) {
		if (size == 6 && data[3] == 0x36) {
			state->speed = (int16_t)sys_get_le16(data + 4);
			state->fields |= BP_LWP3_HAS_SPEED;
			state->speed_ms = now;
		} else if (size == 6 && data[3] == 0x33) {
			state->color_raw[0] = sys_get_le16(data + 4);
			state->fields = (state->fields & ~BP_LWP3_HAS_RGB) | BP_LWP3_HAS_TAG;
			state->color_ms = now;
		}
	}
}

static pbio_color_t duplo_tag_color(uint16_t tag)
{
	switch (tag) {
	case 1: return PBIO_COLOR_WHITE;
	case 24: return PBIO_COLOR_YELLOW;
	case 119: return PBIO_COLOR_CYAN;
	case 321: return PBIO_COLOR_BLUE;
	case 324: return PBIO_COLOR_VIOLET;
	case 353: return PBIO_COLOR_MAGENTA;
	case 28: return PBIO_COLOR_GREEN;
	case 21: return PBIO_COLOR_RED;
	default: return PBIO_COLOR_NONE;
	}
}

static pbio_color_t duplo_rgb_color(pbio_color_t hsv)
{
	uint16_t h = pbio_color_get_h(hsv);
	uint8_t s = pbio_color_get_s(hsv);
	int8_t v = pbio_color_get_v(hsv);
	if (v > 30 && h > 235 && h < 270 && s < 50) return PBIO_COLOR_VIOLET;
	if (v > 60 && h > 190 && h < 230 && s < 40) return PBIO_COLOR_MAGENTA;
	if (h > 70 && h < 110 && s > 50) return PBIO_COLOR_CYAN;
	if (s < 50) return v > 75 ? PBIO_COLOR_WHITE : PBIO_COLOR_NONE;
	if (h > 300 || h < 20) return PBIO_COLOR_RED;
	if (h < 85) return PBIO_COLOR_YELLOW;
	return h < 180 ? PBIO_COLOR_GREEN : PBIO_COLOR_BLUE;
}

void lwp3_device_color(struct bp_lwp3_state *state, const struct bp_sensor_palette *palette)
{
	if (state->kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLUE) {
		if (!(state->fields & BP_LWP3_HAS_TAG)) return;
		state->matched_color = duplo_tag_color(state->color_raw[0]);
		state->fields |= BP_LWP3_HAS_MATCHED_COLOR;
		return;
	}
	if (!(state->fields & BP_LWP3_HAS_RGB)) return;
	pbio_color_rgb_t rgb;
	if (state->kind == LWP3_HUB_KIND_DUPLO_TRAIN_BLACK) {
		/* The old train's RGB channels are 16-bit values with a 1900 max. */
		rgb.r = MIN(state->color_raw[0], 1900) * 255 / 1900;
		rgb.g = MIN(state->color_raw[1], 1900) * 255 / 1900;
		rgb.b = MIN(state->color_raw[2], 1900) * 255 / 1900;
		state->hsv = pbio_color_from_rgb(&rgb);
		state->matched_color = duplo_rgb_color(state->hsv);
		state->fields |= BP_LWP3_HAS_MATCHED_COLOR;
		return;
	}
	if (state->kind != LWP3_HUB_KIND_MARIO && state->kind != LWP3_HUB_KIND_LUIGI &&
	    state->kind != LWP3_HUB_KIND_PEACH) return;
	rgb = (pbio_color_rgb_t){.r = state->color_raw[0], .g = state->color_raw[1],
		.b = state->color_raw[2]};
	state->hsv = pbio_color_from_rgb_with_hue_shift(&rgb);
	state->matched_color = PBIO_COLOR_NONE;
	if (palette && palette->count && palette->count <= BP_SENSOR_PALETTE_SIZE) {
		pbio_color_t colors[BP_SENSOR_PALETTE_SIZE];
		for (uint32_t i = 0; i < palette->count; i++) colors[i] = palette->colors[i];
		pbio_color_map_t map;
		pbio_color_map_set(&map, colors, palette->count);
		state->matched_color = pbio_color_map_find(&map, state->hsv);
		state->fields |= BP_LWP3_HAS_MATCHED_COLOR;
	}
}
