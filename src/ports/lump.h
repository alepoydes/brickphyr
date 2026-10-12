// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2025 The Pybricks Authors
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <lego/lump.h>
#include <brickphyr_port.h>

/* Selected protocol state from PBIO port_lump, independent of scheduling and
 * UART I/O. Exactly one trusted port worker owns each instance. */
enum lump_phase { LUMP_TYPE, LUMP_INFO, LUMP_ACK, LUMP_DATA };
struct lump {
	enum lump_phase phase;
	struct bp_port_mode modes[LUMP_MAX_EXT_MODE + 1];
	uint32_t baud, bad_frames, sequence;
	uint32_t firmware_version, hardware_version;
	uint16_t names, formats;
	uint8_t id, num_modes, num_view_modes, ext_mode, capabilities, mode, length;
	uint8_t data[LUMP_MAX_MSG_SIZE];
	uint8_t message[LUMP_MAX_MSG_SIZE + 3];
	uint8_t used, needed;
	bool got_speed, got_version;
};

void lump_reset(struct lump *lump);
/* One byte; 1 means a new DATA sample, 0 means pending/metadata, negative errno
 * means restart the connection. Invalid DATA checksums count and are discarded. */
int lump_receive(struct lump *lump, uint8_t byte);
/* Padded/checksummed command, or -EINVAL. out has at least 34 bytes. */
int lump_command(uint8_t *out, uint8_t command, const uint8_t *data, size_t length);
/* EXT_MODE followed by padded/checksummed DATA; out has at least 37 bytes. */
int lump_data(uint8_t *out, uint8_t mode, const uint8_t *data, size_t length);
