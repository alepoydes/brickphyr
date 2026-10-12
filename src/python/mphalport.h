// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Platform hooks use the declarations in py/mphal.h. */
void mp_hal_set_interrupt_char(int value);
void mp_hal_stdio_mode_raw(void);
void mp_hal_stdio_mode_orig(void);
