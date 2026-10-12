// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2025 The Pybricks Authors
/* Ported from lib/pbio/src/port_lump/protocol.c at a7b1b04ae. Scheduling, UART
 * access and device-specific processing live outside this parser. Every field
 * is length-checked before access; complete mode descriptions precede ACK. */
#include "lump.h"
#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(sizeof(((struct bp_port_mode *)0)->name) == LUMP_MAX_NAME_SIZE + 1);
BUILD_ASSERT(sizeof(((struct bp_port_mode *)0)->unit) == LUMP_MAX_UOM_SIZE + 1);
BUILD_ASSERT(sizeof(((struct bp_port_mode *)0)->combinations) == LUMP_MAX_MSG_SIZE);
BUILD_ASSERT(sizeof(((struct bp_port_mode *)0)->extra[0]) == LUMP_MAX_MSG_SIZE);
BUILD_ASSERT(ARRAY_SIZE(((struct bp_port_mode *)0)->extra) == LUMP_INFO_UNK12 - LUMP_INFO_UNK7 + 1);
BUILD_ASSERT(sizeof(float) == 4);

void lump_reset(struct lump *l)
{
	*l = (struct lump){.baud = 115200};
}

static size_t padded(size_t size)
{
	size_t length = 1;
	while (length < size) {
		length <<= 1;
	}
	return length;
}

static int message(uint8_t *out, uint8_t header, const uint8_t *data, size_t length)
{
	if (!out || !data || !length || length > LUMP_MAX_MSG_SIZE) {
		return -EINVAL;
	}
	size_t size = padded(length);
	for (size_t n = size; n > 1; n >>= 1) {
		header += 1 << 3;
	}
	out[0] = header;
	uint8_t checksum = 0xff ^ header;
	for (size_t i = 0; i < size; i++) {
		out[i + 1] = i < length ? data[i] : 0;
		checksum ^= out[i + 1];
	}
	out[size + 1] = checksum;
	return size + 2;
}

int lump_command(uint8_t *out, uint8_t command, const uint8_t *data, size_t length)
{
	if (command > LUMP_MSG_CMD_MASK) return -EINVAL;
	return message(out, LUMP_MSG_TYPE_CMD | command, data, length);
}

int lump_data(uint8_t *out, uint8_t mode, const uint8_t *data, size_t length)
{
	if (!out || mode > LUMP_MAX_EXT_MODE) return -EINVAL;
	int size = message(out + 3, LUMP_MSG_TYPE_DATA | (mode & LUMP_MSG_CMD_MASK), data, length);
	if (size < 0) return size;
	uint8_t extension = mode > LUMP_MAX_MODE ? 8 : 0;
	lump_command(out, LUMP_CMD_EXT_MODE, &extension, 1);
	return size + 3;
}

static int parse(struct lump *l)
{
	uint8_t *message = l->message;
	uint8_t type = message[0] & LUMP_MSG_TYPE_MASK;
	uint8_t command = message[0] & LUMP_MSG_CMD_MASK;
	size_t length = LUMP_MSG_SIZE(message[0]);
	uint8_t checksum = 0xff;
	if (type != LUMP_MSG_TYPE_SYS) {
		for (size_t i = 0; i < l->needed; i++) {
			checksum ^= message[i];
		}
		if (checksum) {
			l->bad_frames += l->bad_frames != UINT32_MAX;
			return l->phase == LUMP_TYPE || l->phase == LUMP_DATA ? 0 : -EBADMSG;
		}
	}
	uint8_t *data = message + 1;
	if (l->phase == LUMP_TYPE) {
		/* A framed, checksummed TYPE is an opaque byte. Recognition belongs
		 * to the device service; a private device ID must still be inspectable. */
		l->id = data[0];
		l->phase = LUMP_INFO;
		return 0;
	}
	if (type == LUMP_MSG_TYPE_SYS) {
		if (message[0] == LUMP_SYS_SYNC) {
			return 0;
		}
		if (message[0] != LUMP_SYS_ACK || l->phase != LUMP_INFO || !l->num_modes ||
		    l->names != BIT_MASK(l->num_modes) || l->formats != l->names) {
			return -EBADMSG;
		}
		l->phase = LUMP_ACK;
		return 0;
	}
	if (type == LUMP_MSG_TYPE_CMD) {
		switch (command) {
		case LUMP_CMD_EXT_MODE:
			if (length != 1 || (data[0] != 0 && data[0] != 8)) {
				return -EBADMSG;
			}
			l->ext_mode = data[0];
			return 0;
		case LUMP_CMD_WRITE:
			return 0;
		case LUMP_CMD_MODES:
			if (l->phase != LUMP_INFO || l->num_modes || data[0] > LUMP_MAX_MODE ||
			    (length != 1 && length != 2 && length != 4)) {
				return -EBADMSG;
			}
			l->num_modes = (length >= 4 ? data[2] : data[0]) + 1;
			l->num_view_modes = (length == 4 ? data[3] : length == 2 ? data[1] : data[0]) + 1;
			return l->num_modes && l->num_modes <= ARRAY_SIZE(l->modes) &&
				l->num_view_modes && l->num_view_modes <= l->num_modes ? 0 : -EBADMSG;
		case LUMP_CMD_SPEED:
			if (l->phase != LUMP_INFO || l->got_speed || length != 4) {
				return -EBADMSG;
			}
			l->baud = sys_get_le32(data);
			l->got_speed = true;
			return l->baud >= 2400 && l->baud <= 460800 ? 0 : -EBADMSG;
		case LUMP_CMD_VERSION:
			if (l->phase != LUMP_INFO || l->got_version || length != 8) {
				return -EBADMSG;
			}
			l->got_version = true;
			l->firmware_version = sys_get_le32(data);
			l->hardware_version = sys_get_le32(data + 4);
			return 0;
		default:
			return -EBADMSG;
		}
	}
	uint8_t mode = command + l->ext_mode;
	uint8_t info = message[1];
	if (type == LUMP_MSG_TYPE_INFO && (info & LUMP_INFO_MODE_PLUS_8)) {
		mode = command + 8;
		info &= ~LUMP_INFO_MODE_PLUS_8;
	}
	if (mode >= l->num_modes) {
		return -EBADMSG;
	}
	struct bp_port_mode *m = &l->modes[mode];
	if (type == LUMP_MSG_TYPE_DATA) {
		if (l->phase != LUMP_DATA) {
			return -EBADMSG;
		}
		size_t bytes = m->num_values << MIN(m->data_type, 2);
		if (!bytes || padded(bytes) != length) {
			return -EBADMSG;
		}
		memcpy(l->data, data, bytes);
		l->length = bytes;
		l->mode = mode;
		l->sequence++;
		return 1;
	}
	if (l->phase != LUMP_INFO) {
		return -EBADMSG;
	}
	data++;
	if (info != LUMP_INFO_NAME && !(l->names & BIT(mode))) return -EBADMSG;
	switch (info) {
	case LUMP_INFO_NAME: {
		const uint8_t *end = memchr(data, 0, length);
		size_t size = end ? (size_t)(end - data) : length;
		if (!size || size > LUMP_MAX_NAME_SIZE ||
		    (l->names & BIT(mode))) {
			return -EBADMSG;
		}
		memcpy(m->name, data, size);
		m->name[size] = 0;
		l->names |= BIT(mode);
		if (size <= LUMP_MAX_SHORT_NAME_SIZE && length >= 12) {
			memcpy(m->flags, data + 6, sizeof(m->flags));
			m->present |= BP_PORT_MODE_FLAGS;
			l->capabilities |= m->flags[0];
		}
		break;
	}
	case LUMP_INFO_FORMAT:
		if (length != 4 || !(l->names & BIT(mode)) || (l->formats & BIT(mode)) ||
		    !data[0] || data[1] > LUMP_DATA_TYPE_DATAF ||
		    ((unsigned int)data[0] << MIN(data[1], 2)) > LUMP_MAX_MSG_SIZE) {
			return -EBADMSG;
		}
		m->num_values = data[0];
		m->data_type = data[1];
		m->digits = data[2];
		m->decimals = data[3];
		l->formats |= BIT(mode);
		break;
	case LUMP_INFO_MAPPING:
		if (length != 2 || (m->present & BP_PORT_MODE_MAPPING)) {
			return -EBADMSG;
		}
		m->input_mapping = data[0];
		m->output_mapping = data[1];
		m->present |= BP_PORT_MODE_MAPPING;
		break;
	case LUMP_INFO_RAW:
	case LUMP_INFO_PCT:
	case LUMP_INFO_SI: {
		uint8_t bit = info == LUMP_INFO_RAW ? BP_PORT_MODE_RAW :
			info == LUMP_INFO_PCT ? BP_PORT_MODE_PERCENT : BP_PORT_MODE_SI;
		if (length != 8 || (m->present & bit)) return -EBADMSG;
		float *range = info == LUMP_INFO_RAW ? m->raw : info == LUMP_INFO_PCT ? m->percent : m->si;
		for (size_t i = 0; i < 2; i++) {
			uint32_t bits = sys_get_le32(data + 4 * i);
			memcpy(&range[i], &bits, sizeof(bits));
		}
		m->present |= bit;
		break;
	}
	case LUMP_INFO_UNITS: {
		const uint8_t *end = memchr(data, 0, length);
		size_t size = end ? (size_t)(end - data) : length;
		if (size > LUMP_MAX_UOM_SIZE || (m->present & BP_PORT_MODE_UNIT)) return -EBADMSG;
		memcpy(m->unit, data, size);
		m->unit[size] = 0;
		m->present |= BP_PORT_MODE_UNIT;
		break;
	}
	case LUMP_INFO_MODE_COMBOS:
		if (length < 2 || (m->present & BP_PORT_MODE_COMBINATIONS)) return -EBADMSG;
		m->num_combinations = length / 2;
		for (size_t i = 0; i < m->num_combinations; i++) {
			m->combinations[i] = sys_get_le16(data + 2 * i);
		}
		m->present |= BP_PORT_MODE_COMBINATIONS;
		break;
	default:
		if (info >= LUMP_INFO_UNK7 && info <= LUMP_INFO_UNK12) {
			uint8_t index = info - LUMP_INFO_UNK7;
			if (m->extra_length[index]) return -EBADMSG;
			m->extra_length[index] = length;
			memcpy(m->extra[index], data, length);
		} else {
			m->other_info += m->other_info != UINT16_MAX;
		}
		break;
	}
	return 0;
}

int lump_receive(struct lump *l, uint8_t byte)
{
	if (!l->used) {
		if (l->phase == LUMP_TYPE && byte != (LUMP_MSG_TYPE_CMD | LUMP_CMD_TYPE)) {
			return 0;
		}
		uint8_t type = byte & LUMP_MSG_TYPE_MASK;
		if (type == LUMP_MSG_TYPE_SYS) {
			l->needed = 1;
		} else {
			size_t size = LUMP_MSG_SIZE(byte);
			if (size > LUMP_MAX_MSG_SIZE) {
				return -EMSGSIZE;
			}
			l->needed = size + (type == LUMP_MSG_TYPE_INFO ? 3 : 2);
		}
		/* Reacquire framing after invalid DATA headers, as in PBIO. */
		if (l->phase == LUMP_DATA && type != LUMP_MSG_TYPE_DATA &&
		    (type != LUMP_MSG_TYPE_CMD || ((byte & LUMP_MSG_CMD_MASK) != LUMP_CMD_WRITE &&
		     (byte & LUMP_MSG_CMD_MASK) != LUMP_CMD_EXT_MODE))) {
			return 0;
		}
	}
	l->message[l->used++] = byte;
	if (l->used < l->needed) {
		return 0;
	}
	l->used = 0;
	return parse(l);
}
