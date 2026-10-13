// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <brickphyr_lwp3.h>

bool lwp3_device_matches(const struct bp_lwp3_config *config, uint8_t kind);
bool lwp3_device_pair(const struct bp_lwp3_config *config, uint8_t kind);
/* Return message size, zero after the last setup step, or a negative errno. */
int lwp3_device_setup(uint8_t profile, uint8_t kind, uint8_t step, uint8_t data[BP_LWP3_MESSAGE_MAX]);
int lwp3_device_command(uint8_t profile, uint8_t kind, uint32_t command,
	int32_t value, int32_t auxiliary, uint8_t data[BP_LWP3_MESSAGE_MAX]);
/* Only update fields for the selected kind, with exact message/header sizes.
 * Numeric color conversion is deferred to get(), outside the RX spinlock. */
void lwp3_device_notify(struct bp_lwp3_state *state, const uint8_t *data, size_t size, int64_t now);
void lwp3_device_color(struct bp_lwp3_state *state, const struct bp_sensor_palette *palette);
