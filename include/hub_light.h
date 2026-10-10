// SPDX-License-Identifier: Apache-2.0
#ifndef BRICKPHYR_HUB_LIGHT_H_
#define BRICKPHYR_HUB_LIGHT_H_

#include <stdbool.h>
#include <stdint.h>
#include "host_transport.h"

/* Advance readiness lighting; return next uptime deadline or INT64_MAX if stopped. */
int64_t hub_light_update(int64_t now);
/* Preview or display an interface; white Bluetooth lighting marks an open menu. */
int hub_light_select(enum host_mode selected, bool menu, int64_t now);
/* RGB uses gamma 2 and board white balance; PWM bypasses both transformations. */
int hub_light_manual(bool center, const uint8_t rgb[3]);
int hub_light_pwm(bool center, const uint16_t duty[3]);
/* Stop animation and reject further manual writes, then extinguish all button LEDs. */
int hub_light_off(void);

#endif
