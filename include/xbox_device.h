// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <brickphyr_xbox.h>
/* BLE report 1 excludes the report ID. No state is changed for malformed data. */
int xbox_device_input(struct bp_xbox_state *state, const uint8_t *data,
	size_t size, uint8_t deadzone, int64_t now);
/* Return 8-byte report-3 payload size, zero for no-op, or a negative errno. */
int xbox_device_rumble(const struct bp_xbox_rumble *rumble, uint8_t data[8]);
