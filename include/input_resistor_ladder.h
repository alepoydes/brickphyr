// SPDX-License-Identifier: Apache-2.0
#ifndef BRICKPHYR_INPUT_RESISTOR_LADDER_H_
#define BRICKPHYR_INPUT_RESISTOR_LADDER_H_

#include <zephyr/device.h>

struct resistor_ladder_sample {
	int64_t sampled_ms;
	int16_t raw;
	int error;
	uint32_t failures;
	uint8_t pressed; /* Debounced bits in devicetree key-codes order. */
	bool stable;
};

/* Last conversion, including errors; does not start another ADC transaction. */
struct resistor_ladder_sample resistor_ladder_get_sample(const struct device *dev);
/* Interpret a key from the same captured conversion as the other observations. */
int resistor_ladder_sample_key(const struct device *dev,
	const struct resistor_ladder_sample *sample, uint16_t code);
/* Debounced key: 0 released, 1 pressed, negative errno if unreadable or unstable. */
int resistor_ladder_get_key(const struct device *dev, uint16_t code);

#endif
