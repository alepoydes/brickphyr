// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef CONFIG_BRICKPHYR_WATCHDOG
/* Transition initialization supervision to normal hub-loop supervision. */
int hub_watchdog_run(void);
/* Called by the hub loop, including its USB-powered off state. */
void hub_watchdog_feed(void);
#else
static inline int hub_watchdog_run(void) { return 0; }
static inline void hub_watchdog_feed(void) { }
#endif
