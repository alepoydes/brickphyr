// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <errno.h>
#include <stddef.h>
#include <brickphyr_matrix.h>

#ifdef CONFIG_BRICKPHYR_MATRIX
/* Copies into one firmware-owned pending frame. Never waits or allocates.
 * The first accepted write acquires ownership; an occupied frame returns EBUSY.
 */
int hub_matrix_write(enum bp_matrix_owner owner, uint32_t first, const uint8_t *values,
		     size_t count);
int hub_matrix_get(struct bp_matrix_state *state);
/* Only privileged callers may wait. Do not call from the system work queue. */
int hub_matrix_wait(void);
int hub_matrix_release(enum bp_matrix_owner owner);
/* May run under the host transport spinlock; release continues asynchronously. */
void hub_matrix_session_closed(void);
int hub_matrix_shutdown(void);
#else
static inline int hub_matrix_write(enum bp_matrix_owner owner, uint32_t first,
	const uint8_t *values, size_t count) { return -ENOTSUP; }
static inline int hub_matrix_get(struct bp_matrix_state *state) { return -ENOTSUP; }
static inline int hub_matrix_release(enum bp_matrix_owner owner) { return 0; }
static inline void hub_matrix_session_closed(void) { }
static inline int hub_matrix_shutdown(void) { return 0; }
#endif
