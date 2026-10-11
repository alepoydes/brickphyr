// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

struct sound_waveform {
	uint32_t frequency, rate, phase;
	uint16_t low, high;
};
void sound_waveform_init(struct sound_waveform *wave, uint32_t frequency, uint32_t rate,
	uint32_t volume);
void sound_waveform_fill(struct sound_waveform *wave, uint16_t *dest, size_t count);
