// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "configuration.h"

#include <stdbool.h>
#include <stdint.h>

extern const struct config_entry uart_config[1];
bool hub_uart_power_allowed(void);
int hub_uart_allow_power(uint32_t allowed);
