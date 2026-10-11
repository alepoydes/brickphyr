// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <stddef.h>
#include <brickphyr_sound.h>

#ifdef CONFIG_BRICKPHYR_SOUND
/* Nonblocking commands. Each accepted stream/tone acquires the free speaker;
 * completion/stop releases it. Samples are copied before write returns.
 * Write accepts the whole chunk or EAGAIN; it never partially consumes it.
 */
int hub_sound_tone(enum bp_sound_owner owner, uint32_t frequency, uint32_t duration_ms, uint32_t volume);
int hub_sound_stream(enum bp_sound_owner owner, uint32_t rate);
int hub_sound_write(enum bp_sound_owner owner, const int16_t *samples, size_t count);
int hub_sound_finish(enum bp_sound_owner owner);
int hub_sound_stop(enum bp_sound_owner owner);
int hub_sound_get(struct bp_sound_state *state);
/* Privileged callers only; not from the system workqueue. */
int hub_sound_release(enum bp_sound_owner owner);
void hub_sound_session_closed(void);
int hub_sound_shutdown(void);
#else
static inline int hub_sound_tone(enum bp_sound_owner owner, uint32_t frequency,
	uint32_t duration_ms, uint32_t volume) { return -ENOTSUP; }
static inline int hub_sound_stream(enum bp_sound_owner owner, uint32_t rate) { return -ENOTSUP; }
static inline int hub_sound_write(enum bp_sound_owner owner, const int16_t *samples,
	size_t count) { return -ENOTSUP; }
static inline int hub_sound_finish(enum bp_sound_owner owner) { return -ENOTSUP; }
static inline int hub_sound_stop(enum bp_sound_owner owner) { return -ENOTSUP; }
static inline int hub_sound_get(struct bp_sound_state *state) { return -ENOTSUP; }
static inline int hub_sound_release(enum bp_sound_owner owner) { return 0; }
static inline void hub_sound_session_closed(void) { }
static inline int hub_sound_shutdown(void) { return 0; }
#endif
