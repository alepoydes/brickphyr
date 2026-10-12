// SPDX-License-Identifier: Apache-2.0
#ifndef BRICKPHYR_HUB_LIGHT_H_
#define BRICKPHYR_HUB_LIGHT_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <brickphyr_light.h>
#include "host_transport.h"

/* Advance readiness lighting; return next uptime deadline or INT64_MAX if stopped. */
int64_t hub_light_update(int64_t now, enum host_ble_status bluetooth);
enum hub_light_menu { HUB_LIGHT_NORMAL, HUB_LIGHT_MENU, HUB_LIGHT_BUSY };
/* Preview an interface; menu white and busy blanking override BLE indication. */
int hub_light_select(enum host_mode selected, enum hub_light_menu menu, int64_t now);
/* RGB uses gamma 2 and board white balance; PWM bypasses both transformations. */
int hub_light_manual(bool center, const uint8_t rgb[3]);
int hub_light_pwm(bool center, const uint16_t duty[3]);
/* Stop animation and reject further manual writes, then extinguish all button LEDs. */
int hub_light_off(void);

#ifdef CONFIG_BRICKPHYR_LIGHT
int hub_light_write(uint32_t light, const uint8_t rgb[3]);
int hub_light_animate(uint32_t light, const struct bp_light_frame *frames, size_t count);
int hub_light_get(uint32_t light, struct bp_light_state *state);
int hub_light_release(void);
#else
static inline int hub_light_write(uint32_t light, const uint8_t rgb[3]) { return -ENOTSUP; }
static inline int hub_light_animate(uint32_t light, const struct bp_light_frame *frames,
	size_t count) { return -ENOTSUP; }
static inline int hub_light_get(uint32_t light, struct bp_light_state *state) { return -ENOTSUP; }
static inline int hub_light_release(void) { return 0; }
#endif

#endif
