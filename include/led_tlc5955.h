// SPDX-License-Identifier: Apache-2.0
#ifndef BRICKPHYR_LED_TLC5955_H_
#define BRICKPHYR_LED_TLC5955_H_

#include <zephyr/device.h>

/* Driver extension: latch consecutive RGB LEDs together at native 16-bit duty. */
int tlc5955_set_colors16(const struct device *dev, uint32_t first, uint32_t count,
			const uint16_t *colors);

/* Privileged sparse PWM write, sharing the RGB frame and latch. Reserved
 * initial-full channels cannot be changed through the LED interface. */
int tlc5955_set_channels16(const struct device *dev, const uint8_t *channels,
			  const uint16_t *values, size_t count);

#endif
