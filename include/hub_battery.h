// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <brickphyr_battery.h>

#ifdef CONFIG_BRICKPHYR_BATTERY
int hub_battery_get(struct bp_battery_sample *sample);
/* Privileged shutdown joins acquisition; button ADC channels remain available. */
void hub_battery_shutdown(void);
#else
static inline int hub_battery_get(struct bp_battery_sample *sample) { return -ENOTSUP; }
static inline void hub_battery_shutdown(void) { }
#endif
