// SPDX-License-Identifier: Apache-2.0
#ifndef BRICKPHYR_HUB_LIGHT_H_
#define BRICKPHYR_HUB_LIGHT_H_

#include <stdbool.h>
#include <stdint.h>

/* Advance readiness lighting; return next uptime deadline or INT64_MAX if stopped. */
int64_t hub_light_update(int64_t now);
int hub_light_manual(bool center, const uint8_t rgb[3]);
/* Stop animation and reject further manual writes, then extinguish all button LEDs. */
int hub_light_off(void);

#endif
