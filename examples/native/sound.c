// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

static int drain(void)
{
	for (;;) {
		struct bp_sound_state state;
		int err = bp_sound_get(&state);
		if (err || state.error) {
			return err ? err : state.error;
		}
		if (state.phase == BP_SOUND_IDLE) {
			return 0;
		}
		bp_sleep(1);
	}
}

int app_main(void)
{
	int err = bp_sound_tone(440, 200, 10);
	if (!err) {
		err = drain();
	}
	if (!err) {
		err = bp_sound_stream(16000);
	}
	if (err) {
		return err;
	}
	/* A quiet 500 Hz triangle demonstrates PCM without a file or allocation. */
	int16_t samples[BP_SOUND_CHUNK_SAMPLES];
	for (size_t i = 0; i < BP_SOUND_CHUNK_SAMPLES; i++) {
		unsigned int phase = i % 32;
		samples[i] = (phase < 16 ? phase : 32 - phase) * 256 - 2048;
	}
	for (unsigned int chunk = 0; chunk < 32;) {
		err = bp_sound_write(samples, BP_SOUND_CHUNK_SAMPLES);
		if (err == -EAGAIN) {
			bp_sleep(1);
		} else if (err) {
			return err;
		} else {
			chunk++;
		}
	}
	err = bp_sound_finish();
	return err ? err : drain();
}
