// SPDX-License-Identifier: Apache-2.0
#include "application_private.h"
#include <errno.h>
#include "py/runtime.h"
#include "py/persistentcode.h"

void *python_native_commit(void *buffer, size_t length, void *relocation)
{
	intptr_t address = application_code_reserve(length);
	if (address < 0) {
		if (address == -ENOMEM) {
			mp_raise_msg_varg(&mp_type_MemoryError,
				MP_ERROR_TEXT("native code reservation failed: %u bytes"), (unsigned)length);
		}
		mp_raise_OSError(-address);
	}
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		if (relocation) {
			mp_native_relocate(relocation, buffer, address);
		}
		int err = application_code_commit(buffer, length);
		if (err) {
			mp_raise_OSError(-err);
		}
		nlr_pop();
		return (void *)address;
	}
	/* Relocation can raise on malformed input. Keep its first exception. */
	(void)application_code_commit(NULL, 0);
	nlr_jump(nlr.ret_val);
}
