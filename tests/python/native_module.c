// SPDX-License-Identifier: Apache-2.0
#include "py/dynruntime.h"

/* dynruntime relocates BSS through externally visible symbols. */
mp_int_t calls;
static const mp_int_t offsets[] = {3, 9, 12};

static mp_obj_t add(mp_obj_t value)
{
	calls++;
	return mp_obj_new_int(mp_obj_get_int(value) + offsets[calls % 3]);
}
static MP_DEFINE_CONST_FUN_OBJ_1(add_object, add);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args)
{
	MP_DYNRUNTIME_INIT_ENTRY
	if (calls) {
		mp_raise_ValueError("BSS was not initialized");
	}
	mp_store_global(MP_QSTR_add, MP_OBJ_FROM_PTR(&add_object));
	mp_obj_t gc = mp_import_name(MP_QSTR_gc, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
	mp_call_function_n_kw(mp_load_attr(gc, MP_QSTR_collect), 0, 0, NULL);
	mp_obj_t argument = mp_obj_new_int(33);
	mp_obj_t result = mp_call_function_n_kw(MP_OBJ_FROM_PTR(&add_object), 1, 0, &argument);
	mp_printf(&mp_plat_print, "native C %d %d\n", (int)mp_obj_get_int(result), (int)calls);
	MP_DYNRUNTIME_INIT_EXIT
}
