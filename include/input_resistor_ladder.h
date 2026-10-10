// SPDX-License-Identifier: Apache-2.0
#ifndef BRICKPHYR_INPUT_RESISTOR_LADDER_H_
#define BRICKPHYR_INPUT_RESISTOR_LADDER_H_

#include <zephyr/device.h>

struct resistor_ladder_sample {
	int16_t raw;
	int error;
	uint32_t failures;
};

/* Last conversion, including errors; does not start another ADC transaction. */
struct resistor_ladder_sample resistor_ladder_get_sample(const struct device *dev);
/* Debounced key: 0 released, 1 pressed, negative errno if unreadable or unstable. */
int resistor_ladder_get_key(const struct device *dev, uint16_t code);

#endif
