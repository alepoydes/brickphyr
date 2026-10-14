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
struct lump_mode {
	float raw[2], percent[2], si[2];
	/* COMBOS followed by INFO 0x07..0x0c in index order. Only advertised
	 * bytes are allocated; lengths include the device's wire padding. */
	uint8_t *payload;
	uint16_t other_info;
	char name[LUMP_MAX_NAME_SIZE + 1], unit[LUMP_MAX_UOM_SIZE + 1];
	uint8_t flags[6], lengths[LUMP_INFO_UNK12 - LUMP_INFO_UNK7 + 2];
	uint8_t present, num_values, data_type, digits, decimals;
	uint8_t input_mapping, output_mapping;
};
struct lump {
	enum lump_phase phase;
	/* Allocated once from the advertised mode count, immutable after ACK. */
	struct lump_mode *modes;
	uint32_t baud, bad_frames, sequence;
	uint32_t firmware_version, hardware_version;
	uint16_t names, formats;
	uint8_t id, num_modes, num_view_modes, ext_mode, capabilities, mode, length;
	uint8_t data[LUMP_MAX_MSG_SIZE];
	uint8_t message[LUMP_MAX_MSG_SIZE + 3];
	uint8_t used, needed;
	bool got_speed, got_version;
};

/* Initially zero-initialize the instance. Reset releases its old metadata. */
void lump_reset(struct lump *lump);
/* Release metadata after readers are invalidated, retaining diagnostics. */
void lump_free_modes(struct lump *lump);
/* Expand one immutable mode into the public ABI without allocation or I/O. */
void lump_mode_get(const struct lump_mode *mode, struct bp_port_mode *value);
/* One byte; 1 means a new DATA sample, 0 means pending/metadata, negative errno
 * means restart the connection. Invalid DATA checksums count and are discarded. */
int lump_receive(struct lump *lump, uint8_t byte);
/* Padded/checksummed command, or -EINVAL. out has at least 34 bytes. */
int lump_command(uint8_t *out, uint8_t command, const uint8_t *data, size_t length);
/* EXT_MODE followed by padded/checksummed DATA; out has at least 37 bytes. */
int lump_data(uint8_t *out, uint8_t mode, const uint8_t *data, size_t length);
