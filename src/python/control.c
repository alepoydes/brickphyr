// SPDX-License-Identifier: Apache-2.0
#include "application_private.h"
#include "py/runtime.h"

static void check(int result)
{
	if (result < 0) mp_raise_OSError(-result);
}

static mp_obj_t control_start(size_t count, const mp_obj_t *args)
{
	mp_buffer_info_t image, parameters = {0};
	mp_get_buffer_raise(args[0], &image, MP_BUFFER_READ);
	if (count == 2) mp_get_buffer_raise(args[1], &parameters, MP_BUFFER_READ);
	check(application_control_start(image.buf, image.len, parameters.buf, parameters.len));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(control_start_obj, 1, 2, control_start);

static mp_obj_t control_update(mp_obj_t data)
{
	mp_buffer_info_t parameters;
	mp_get_buffer_raise(data, &parameters, MP_BUFFER_READ);
	int revision = application_control_update(parameters.buf, parameters.len);
	check(revision);
	return mp_obj_new_int(revision);
}
static MP_DEFINE_CONST_FUN_OBJ_1(control_update_obj, control_update);

static mp_obj_t control_status(void)
{
	struct application_control_status value;
	check(application_control_get(&value));
	mp_obj_t result = mp_obj_new_dict(0);
#define FIELD(name, number) mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_##name), mp_obj_new_int_from_ll(number))
	FIELD(sequence, value.step.sequence);
	FIELD(skipped, value.step.skipped);
	FIELD(scheduled_us, value.step.scheduled_us);
	FIELD(started_us, value.step.started_us);
	FIELD(revision, value.revision);
	FIELD(applied_revision, value.step.parameter_revision);
	FIELD(period_us, value.period_us);
	FIELD(stack_bytes, value.stack_bytes);
	FIELD(loader_bytes, value.loader_bytes);
	FIELD(loaded_bytes, value.loaded_bytes);
#undef FIELD
	return result;
}
static MP_DEFINE_CONST_FUN_OBJ_0(control_status_obj, control_status);

static const mp_rom_map_elem_t globals_table[] = {
	{MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_brickphyr)},
	{MP_ROM_QSTR(MP_QSTR_control_start), MP_ROM_PTR(&control_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_control_update), MP_ROM_PTR(&control_update_obj)},
	{MP_ROM_QSTR(MP_QSTR_control_status), MP_ROM_PTR(&control_status_obj)},
};
static MP_DEFINE_CONST_DICT(globals, globals_table);
const mp_obj_module_t brickphyr_module = {
	.base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&globals,
};
MP_REGISTER_MODULE(MP_QSTR_brickphyr, brickphyr_module);
