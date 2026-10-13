// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <brickphyr_lwp3.h>
#include <brickphyr_xbox.h>

#define BP_BLE_PEER_NAME_MAX 28
enum bp_ble_peer_protocol { BP_BLE_LWP3, BP_BLE_XBOX };
struct bp_ble_peer_config {
	/* Scan timeout in ms, zero indefinite; connection timeout is 5 s. */
	uint32_t timeout_ms;
	uint8_t protocol;
	char name[BP_BLE_PEER_NAME_MAX + 1];
	union {
		struct bp_lwp3_config lwp3;
		uint8_t xbox_deadzone; /* square joystick deadzone, 0..100 percent */
	};
};
struct bp_ble_peer_state {
	int32_t error;
	uint8_t connected, busy;
	char name[BP_BLE_PEER_NAME_MAX + 1];
	union {
		struct bp_lwp3_state lwp3;
		struct bp_xbox_state xbox;
	};
};
