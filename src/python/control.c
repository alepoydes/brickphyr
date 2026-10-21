// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include "py/objint.h"
#include "py/runtime.h"
#include "files.h"
#include "application_private.h"
#include <string.h>

static void check(int result)
{
	if (result < 0) mp_raise_OSError(-result);
}

static mp_obj_t control_start(size_t count, const mp_obj_t *args)
{
	mp_buffer_info_t image, parameters = {0};
	if (count == 2) mp_get_buffer_raise(args[1], &parameters, MP_BUFFER_READ);
	if (mp_obj_is_str(args[0])) {
		size_t length;
		const char *name = mp_obj_str_get_data(args[0], &length);
		if (memchr(name, 0, length)) mp_raise_ValueError(MP_ERROR_TEXT("invalid resource name"));
		check(bp_control_start_resource(name, length, parameters.buf, parameters.len));
	} else {
		mp_get_buffer_raise(args[0], &image, MP_BUFFER_READ);
		check(bp_control_start(image.buf, image.len, parameters.buf, parameters.len));
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(control_start_obj, 1, 2, control_start);

static mp_obj_t control_update(mp_obj_t data)
{
	mp_buffer_info_t parameters;
	mp_get_buffer_raise(data, &parameters, MP_BUFFER_READ);
	int revision = bp_control_update(parameters.buf, parameters.len);
	check(revision);
	return mp_obj_new_int(revision);
}
static MP_DEFINE_CONST_FUN_OBJ_1(control_update_obj, control_update);

static void number_field(mp_obj_t result, qstr name, uint64_t value)
{
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(name), mp_obj_new_int_from_ull(value));
}

static mp_obj_t step_fields(const struct bp_control_step *step)
{
	mp_obj_t result = mp_obj_new_dict(0);
	number_field(result, MP_QSTR_sequence, step->sequence);
	number_field(result, MP_QSTR_skipped, step->skipped);
	number_field(result, MP_QSTR_scheduled_us, step->scheduled_us);
	number_field(result, MP_QSTR_started_us, step->started_us);
	number_field(result, MP_QSTR_applied_revision, step->parameter_revision);
	return result;
}

static mp_obj_t control_status(void)
{
	struct bp_control_status value;
	check(bp_control_get(&value));
	mp_obj_t result = step_fields(&value.step);
	number_field(result, MP_QSTR_revision, value.revision);
	number_field(result, MP_QSTR_period_us, value.period_us);
	number_field(result, MP_QSTR_stack_bytes, value.stack_bytes);
	number_field(result, MP_QSTR_loader_bytes, value.loader_bytes);
	number_field(result, MP_QSTR_loaded_bytes, value.loaded_bytes);
	return result;
}
static MP_DEFINE_CONST_FUN_OBJ_0(control_status_obj, control_status);

static mp_obj_t record_info(void)
{
	struct bp_record_info info;
	check(bp_record_get(&info));
	mp_obj_t result = mp_obj_new_dict(0);
	number_field(result, MP_QSTR_generation, info.generation);
	number_field(result, MP_QSTR_first, info.first);
	number_field(result, MP_QSTR_next, info.next);
	number_field(result, MP_QSTR_parameter_size, info.parameter_size);
	number_field(result, MP_QSTR_size, info.spec.size);
	number_field(result, MP_QSTR_capacity, info.spec.capacity);
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_schema), mp_obj_new_str(info.spec.schema, strlen(info.spec.schema)));
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_control), mp_obj_new_bool(info.control));
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_active), mp_obj_new_bool(info.active));
	return result;
}
static MP_DEFINE_CONST_FUN_OBJ_0(record_info_obj, record_info);

static mp_obj_t record_readinto(size_t count, const mp_obj_t *args)
{
	struct bp_record_request request;
	if (!mp_obj_is_int(args[0]) || !mp_obj_is_int(args[1])) mp_raise_TypeError(MP_ERROR_TEXT("integer cursor required"));
	mp_obj_int_to_bytes(args[0], sizeof(request.generation), (byte *)&request.generation, MP_ENDIANNESS_BIG, false, true);
	mp_obj_int_to_bytes(args[1], sizeof(request.index), (byte *)&request.index, MP_ENDIANNESS_BIG, false, true);
	mp_buffer_info_t data, parameters;
	mp_get_buffer_raise(args[2], &data, MP_BUFFER_WRITE);
	mp_get_buffer_raise(args[3], &parameters, MP_BUFFER_WRITE);
	request.data = data.buf;
	request.capacity = data.len;
	request.parameters = parameters.buf;
	request.parameters_capacity = parameters.len;
	struct bp_record_info info;
	struct bp_control_step step = {0};
	int length = bp_record_readinto(&request, &info, &step);
	check(length);
	if (!length) return mp_const_none;
	mp_obj_t result = info.control ? step_fields(&step) : mp_obj_new_dict(0);
	if (info.control) number_field(result, MP_QSTR_parameter_size, step.parameter_size);
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_control), mp_obj_new_bool(info.control));
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_size), mp_obj_new_int(length));
	return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(record_readinto_obj, 4, 4, record_readinto);

static const mp_rom_map_elem_t globals_table[] = {
	{MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_brickphyr)},
	{MP_ROM_QSTR(MP_QSTR_control_start), MP_ROM_PTR(&control_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_control_update), MP_ROM_PTR(&control_update_obj)},
	{MP_ROM_QSTR(MP_QSTR_control_status), MP_ROM_PTR(&control_status_obj)},
	{MP_ROM_QSTR(MP_QSTR_record_info), MP_ROM_PTR(&record_info_obj)},
	{MP_ROM_QSTR(MP_QSTR_record_readinto), MP_ROM_PTR(&record_readinto_obj)},
};
static MP_DEFINE_CONST_DICT(globals, globals_table);
const mp_obj_module_t brickphyr_module = {
	.base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&globals,
};
MP_REGISTER_MODULE(MP_QSTR_brickphyr, brickphyr_module);
