// SPDX-License-Identifier: Apache-2.0
#include "py/dynruntime.h"

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args)
{
	MP_DYNRUNTIME_INIT_ENTRY
	/* Loaded instructions must be readable/executable but not writable. */
	*(volatile uint16_t *)((uintptr_t)mpy_init & ~1U) = 0;
	MP_DYNRUNTIME_INIT_EXIT
}
