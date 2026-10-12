// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

/* Name capacity includes the terminator and accommodates a Bluetooth name. */
#define BP_SYSTEM_NAME_SIZE 249
struct bp_system_info {
	char name[BP_SYSTEM_NAME_SIZE];
	int32_t reset_error;
	uint32_t reset_cause; /* Zephyr RESET_* flags; invalid if reset_error != 0. */
	int32_t program_id; /* Pybricks slot, 128 for REPL, -1 for named/RAM programs. */
	uint8_t program_start_type; /* Reference: 0 none, 1 boot, 2 hub UI, 3 remote. */
	uint8_t connected_usb, connected_ble;
};
