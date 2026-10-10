// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

enum host_mode { HOST_PYBRICKS, HOST_SHELL };

/* One CDC session; a selection is permitted only between closed sessions. */
int host_transport_init(const struct device *uart, uint16_t hub_variant);
void host_connection_changed(bool open);
void host_interface_status(uint8_t state[8]);
int host_interface_select(uint16_t mode);
