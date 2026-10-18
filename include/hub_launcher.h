// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Called only from the hub loop. Storage and application loading use its
 * existing worker; these functions never wait for flash or the display. */
void hub_launcher_poll(bool available, bool pybricks, unsigned int slot, int64_t now);
void hub_launcher_navigate(int direction);
void hub_launcher_start(int64_t now);
/* Prevent a queued button launch from racing with application shutdown. */
void hub_launcher_shutdown(void);
