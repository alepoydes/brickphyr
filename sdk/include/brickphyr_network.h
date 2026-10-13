// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

#define BP_NETWORK_PEERS 3
#define BP_NETWORK_DATA_MAX 513
#define BP_NETWORK_INBOX_MIN (BP_NETWORK_DATA_MAX + 12)

/* Addresses use display/wire order. FF:FF:FF:FF:FF:FF addresses all peers. */
struct bp_network_message {
	uint8_t address[6];
	uint16_t size;
	uint8_t data[BP_NETWORK_DATA_MAX];
};
struct bp_network_state {
	uint32_t request;
	int32_t error;
	uint8_t busy, count, address[6];
	uint8_t peers[BP_NETWORK_PEERS][6];
	uint32_t dropped, malformed;
};
