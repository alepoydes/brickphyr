// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef CONFIG_BRICKPHYR_CAPTURE
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
