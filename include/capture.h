// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef CONFIG_BRICKPHYR_CAPTURE
/* Called outside the application lifecycle lock. No device is claimed or driven. */
void capture_poll(void);
int capture_shutdown(void);
#else
static inline void capture_poll(void) { }
static inline int capture_shutdown(void) { return 0; }
#endif
