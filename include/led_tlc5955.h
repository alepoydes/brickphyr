// SPDX-License-Identifier: Apache-2.0
#ifndef BRICKPHYR_LED_TLC5955_H_
#define BRICKPHYR_LED_TLC5955_H_

#include <zephyr/device.h>

/* Driver extension: latch consecutive RGB LEDs together at native 16-bit duty. */
int tlc5955_set_colors16(const struct device *dev, uint32_t first, uint32_t count,
			const uint16_t *colors);

#endif
