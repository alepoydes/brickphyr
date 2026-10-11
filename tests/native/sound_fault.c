// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <errno.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	int16_t sample = 0;
	if (bp_sound_write(&sample, SIZE_MAX) != -EINVAL ||
	    bp_sound_write(&sample, 0) != -EINVAL) {
		return -ERANGE;
	}
	int err = bp_sound_stream(8000);
	if (err) {
		return err;
	}
	/* The range check must fault this application before copying into the
	 * privileged queue. Its primed stream must still be released on unload. */
	return bp_sound_write((const int16_t *)0xfffffff0U, BP_SOUND_CHUNK_SAMPLES);
}
