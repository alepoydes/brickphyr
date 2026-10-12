// SPDX-License-Identifier: Apache-2.0
#include "py/dynruntime.h"

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args)
{
	MP_DYNRUNTIME_INIT_ENTRY
	/* A valid Thumb return in writable GC memory must remain non-executable. */
	uint16_t *code = m_new(uint16_t, 1);
	*code = 0x4770; /* bx lr */
	((void (*)(void))((uintptr_t)code | 1U))();
	MP_DYNRUNTIME_INIT_EXIT
}
