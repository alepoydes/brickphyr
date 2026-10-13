// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <zephyr/device.h>
#include <stdint.h>

/* Start the BLE-only management transport and its controller worker once.
 * Copies the preserved factory address; no controller waits occur in init.
 * Radio failures are reported by the bluetooth command, not hidden retries. */
int host_cc2640_init(const struct device *npi, const uint8_t address[6]);
