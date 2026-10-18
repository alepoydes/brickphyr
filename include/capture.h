// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <brickphyr_control.h>
#include <brickphyr_record.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_BRICKPHYR_CAPTURE
struct telemetry_wire_field;
/* Describe an exact capture/2 row for the shared lossless block encoder.
 * Pass NULL to count fields before allocating the caller-owned array. */
int capture_wire_layout(const uint8_t config[BP_CAPTURE_CONFIG_SIZE], size_t row_size,
	struct telemetry_wire_field *wire, size_t capacity);
/* The same row followed by 1..8 application-selected scalar values. */
int capture_app_wire_layout(const uint8_t *config, size_t config_size, size_t row_size,
	struct telemetry_wire_field *wire, size_t capacity);
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
