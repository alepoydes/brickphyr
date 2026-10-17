// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <brickphyr_control.h>
#include <brickphyr_record.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_BRICKPHYR_CAPTURE
/* Pack a cached frame with the same generated fields as diagnostic capture.
 * No hardware read, allocation or persistent pointer. */
int capture_pack_frame(const uint8_t config[BP_CAPTURE_CONFIG_SIZE],
	const struct bp_control_step *step, const struct bp_control_frame *frame,
	void *data, size_t capacity);
/* Called outside the application lifecycle lock. Shutdown joins output cleanup. */
void capture_poll(void);
int capture_shutdown(void);
/* Nonblocking; safe from transport callbacks holding a spinlock. */
void capture_session_closed(void);
#else
static inline void capture_poll(void) { }
static inline int capture_shutdown(void) { return 0; }
static inline void capture_session_closed(void) { }
#endif
