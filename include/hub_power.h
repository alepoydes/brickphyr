// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A host acquisition and expiry claim serialize on the same policy lock. */
bool hub_power_connection(bool connected);
/* An autonomous application's lifetime holds power independently of a host. */
bool hub_power_application(bool running);
/* Explicit shutdown also closes admission, even with an active owner. */
void hub_power_request_shutdown(void);
/* VBUS: 0 absent, 1 present, negative errno unknown. True claims shutdown. */
bool hub_power_update(int vbus, bool activity, int64_t now);
/* Load known keys through the same runtime validation used by host commands. */
int hub_power_load(void);
