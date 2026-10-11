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

int lump_command(uint8_t *out, uint8_t command, const uint8_t *data, size_t length)
{
	if (!out || !data || !length || length > LUMP_MAX_MSG_SIZE || command > LUMP_MSG_CMD_MASK) {
		return -EINVAL;
	}
	size_t size = padded(length);
	uint8_t header = LUMP_MSG_TYPE_CMD | command;
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
		if (data[0] < 29 || data[0] > 101) {
			return 0;
		}
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
			if (l->phase != LUMP_INFO || l->num_modes || data[0] > LUMP_MAX_MODE) {
				return -EBADMSG;
			}
			l->num_modes = (length >= 4 ? data[2] : data[0]) + 1;
			return l->num_modes && l->num_modes <= ARRAY_SIZE(l->modes) ? 0 : -EBADMSG;
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
	struct lump_mode *m = &l->modes[mode];
	if (type == LUMP_MSG_TYPE_DATA) {
		if (l->phase != LUMP_DATA) {
			return -EBADMSG;
		}
		size_t bytes = m->values << MIN(m->type, 2);
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
	switch (info) {
	case LUMP_INFO_NAME: {
		const uint8_t *end = memchr(data, 0, length);
		size_t size = end ? (size_t)(end - data) : length;
		if (!size || size > LUMP_MAX_NAME_SIZE || data[0] < 'A' || data[0] > 'z' ||
		    (l->names & BIT(mode))) {
			return -EBADMSG;
		}
		memcpy(m->name, data, size);
		m->name[size] = 0;
		l->names |= BIT(mode);
		if (size <= LUMP_MAX_SHORT_NAME_SIZE && length >= 12) {
			m->flags = data[6];
			l->capabilities |= m->flags;
		}
		break;
	}
	case LUMP_INFO_FORMAT:
		if (length != 4 || !(l->names & BIT(mode)) || (l->formats & BIT(mode)) ||
		    !data[0] || data[1] > LUMP_DATA_TYPE_DATAF ||
		    ((unsigned int)data[0] << MIN(data[1], 2)) > LUMP_MAX_MSG_SIZE) {
			return -EBADMSG;
		}
		m->values = data[0];
		m->type = data[1];
		l->formats |= BIT(mode);
		break;
	case LUMP_INFO_MAPPING:
		if (length < 2 || !(l->names & BIT(mode))) {
			return -EBADMSG;
		}
		m->writable = data[1] != 0;
		break;
	default:
		/* Scaling/units/combinations are not needed for binary acquisition.
		 * They remain framed and checksum-checked, including unknown INFO. */
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
