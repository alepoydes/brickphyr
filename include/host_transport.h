// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

enum host_mode { HOST_PYBRICKS, HOST_SHELL };

/* One CDC session; external selection is permitted only between closed sessions.
 * Owner commands can request the next mode, applied when their session closes. */
int host_transport_init(const struct device *uart, uint16_t hub_variant);
void host_connection_changed(bool open);
void host_interface_status(uint8_t state[8]);
enum host_mode host_interface_current(void);
int host_interface_select(uint16_t mode);
