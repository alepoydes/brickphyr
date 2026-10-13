// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <zephyr/kernel.h>

/* Shared radio workqueue; immediate requests advance pending work, delayed
 * deadlines never postpone an already queued event. No HCI from the caller. */
void host_radio_schedule(k_timeout_t delay);
int host_radio_error(void);
bool host_radio_starting(void);
/* Radio-worker-only fatal command failure; preserves the first radio error. */
int host_radio_failure(const char *operation, int error);
