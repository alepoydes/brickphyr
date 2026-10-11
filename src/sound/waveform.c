// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023, 2025 The Pybricks Authors
/* Square-wave levels and volume law from beep_sampled.c/pb_type_speaker.c.
 * Advance a phase at the PCM clock instead of changing the DAC clock per tone. */
#include "waveform.h"
#include <math.h>

void sound_waveform_init(struct sound_waveform *wave, uint32_t frequency, uint32_t rate,
	uint32_t volume)
{
	uint16_t amplitude = (powf(10, volume / 100.0f) - 1) / 9 * INT16_MAX;
	*wave = (struct sound_waveform){.frequency = frequency, .rate = rate,
		.low = (INT16_MAX - amplitude) & 0xfff0U,
		.high = (INT16_MAX + amplitude) & 0xfff0U};
}

void sound_waveform_fill(struct sound_waveform *wave, uint16_t *dest, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		dest[i] = wave->phase < wave->rate / 2 ? wave->low : wave->high;
		wave->phase += wave->frequency;
		if (wave->phase >= wave->rate) {
			wave->phase -= wave->rate;
		}
	}
}
