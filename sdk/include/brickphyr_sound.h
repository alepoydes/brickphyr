// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

#define BP_SOUND_CHUNK_SAMPLES 256
#define BP_SOUND_QUEUE_SAMPLES (4 * BP_SOUND_CHUNK_SAMPLES)

enum bp_sound_owner { BP_SOUND_FREE, BP_SOUND_APPLICATION, BP_SOUND_TERMINAL };
enum bp_sound_phase { BP_SOUND_IDLE, BP_SOUND_PRIMING, BP_SOUND_PLAYING,
	BP_SOUND_STOPPING, BP_SOUND_FAILED };
enum bp_sound_mode { BP_SOUND_TONE, BP_SOUND_PCM };

/* completed_samples counts consumed DMA samples, not measured acoustic output.
 * queued_samples excludes the two DMA halves. An underrun stops playback and
 * retains its error; it never loops old PCM or silently changes the sample rate.
 * actual_rate is integer Hz (rounded down) for the timer's chosen divider.
 */
struct bp_sound_state {
	int64_t changed_ms;
	uint64_t completed_samples;
	uint32_t requested_rate, actual_rate;
	uint32_t queued_samples;
	uint32_t frequency, duration_ms, volume;
	uint32_t underruns, failures;
	int32_t error, cleanup_error;
	uint8_t owner, phase, mode, eof;
};
