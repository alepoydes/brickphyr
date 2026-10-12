// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2022 The Pybricks Authors
// Adapted from pybricks/common/pb_type_system.c.
#include "devices.h"
#include <brickphyr.h>
#include <errno.h>
#include <string.h>
#include <zephyr/drivers/hwinfo.h>
#include <pybricks/common.h>
#include <pybricks/parameters.h>
#include <pybricks/parameters/pb_type_button.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include "py/objstr.h"

static mp_obj_t system_name(void)
{
	struct bp_system_info value;
	python_device_check(bp_system_get(&value));
	return mp_obj_new_str(value.name, strlen(value.name));
}
static MP_DEFINE_CONST_FUN_OBJ_0(system_name_obj, system_name);

static unsigned int reset_reason(uint32_t cause)
{
	return cause & RESET_WATCHDOG ? 2 : cause & RESET_SOFTWARE ? 1 : 0;
}

static mp_obj_t system_reset_reason(void)
{
	struct bp_system_info value;
	python_device_check(bp_system_get(&value));
	python_device_check(value.reset_error);
	return MP_OBJ_NEW_SMALL_INT(reset_reason(value.reset_cause));
}
static MP_DEFINE_CONST_FUN_OBJ_0(system_reset_reason_obj, system_reset_reason);

static mp_obj_t system_info(void)
{
	struct bp_system_info value;
	python_device_check(bp_system_get(&value));
	mp_obj_t result = mp_obj_new_dict(6);
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_name), mp_obj_new_str(value.name, strlen(value.name)));
	if (!value.reset_error) {
		mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_reset_reason), MP_OBJ_NEW_SMALL_INT(reset_reason(value.reset_cause)));
	} else if (value.reset_error != -ENOTSUP) {
		python_device_check(value.reset_error);
	}
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_host_connected_usb), mp_obj_new_bool(value.connected_usb));
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_host_connected_ble), mp_obj_new_bool(value.connected_ble));
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_program_id), mp_obj_new_int(value.program_id));
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_program_start_type), MP_OBJ_NEW_SMALL_INT(value.program_start_type));
	return result;
}
static MP_DEFINE_CONST_FUN_OBJ_0(system_info_obj, system_info);

static uint32_t button_mask(mp_obj_t button)
{
	switch (pb_type_button_get_button_flag(button)) {
	case PBIO_BUTTON_CENTER: return 1;
	case PBIO_BUTTON_LEFT: return 2;
	case PBIO_BUTTON_RIGHT: return 4;
	case PBIO_BUTTON_RIGHT_UP: return 8;
	default: mp_raise_ValueError(MP_ERROR_TEXT("expected a hub button"));
	}
}

static mp_obj_t system_set_stop_button(mp_obj_t buttons)
{
	uint32_t mask = 0;
	if (mp_obj_is_true(buttons)) {
		if (mp_obj_get_type(buttons) == mp_obj_get_type(MP_OBJ_FROM_PTR(&pb_type_button))) {
			mask = button_mask(buttons);
		} else {
			mp_obj_t iter = mp_getiter(buttons, NULL), item;
			while ((item = mp_iternext(iter)) != MP_OBJ_STOP_ITERATION) {
				mask |= button_mask(item);
			}
		}
	}
	python_device_check(bp_set_stop_buttons(mask));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(system_set_stop_button_obj, system_set_stop_button);

static mp_obj_t system_shutdown(void)
{
	bp_shutdown();
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(system_shutdown_obj, system_shutdown);

static mp_obj_t system_storage(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_FUNCTION(n_args, args, kw_args,
		PB_ARG_REQUIRED(offset), PB_ARG_DEFAULT_NONE(read), PB_ARG_DEFAULT_NONE(write));
	mp_int_t offset = mp_obj_get_int(offset_in);
	if (offset < 0 || offset > BP_USER_DATA_SIZE) {
		mp_raise_ValueError(MP_ERROR_TEXT("storage offset out of range"));
	}
	if (read_in != mp_const_none && write_in == mp_const_none) {
		mp_int_t size = mp_obj_get_int(read_in);
		if (size < 0 || size > BP_USER_DATA_SIZE - offset) {
			mp_raise_ValueError(MP_ERROR_TEXT("storage read out of range"));
		}
		vstr_t bytes;
		vstr_init_len(&bytes, size);
		int err = bp_user_data_read(offset, bytes.buf, size);
		if (err) {
			vstr_clear(&bytes);
			python_device_check(err);
		}
		return mp_obj_new_bytes_from_vstr(&bytes);
	}
	if (write_in != mp_const_none && read_in == mp_const_none) {
		mp_buffer_info_t buffer;
		mp_get_buffer_raise(write_in, &buffer, MP_BUFFER_READ);
		if (buffer.len > BP_USER_DATA_SIZE - offset) {
			mp_raise_ValueError(MP_ERROR_TEXT("storage write out of range"));
		}
		python_device_check(bp_user_data_write(offset, buffer.buf, buffer.len));
		return mp_const_none;
	}
	mp_raise_TypeError(MP_ERROR_TEXT("set either read or write"));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(system_storage_obj, 0, system_storage);

static const mp_rom_map_elem_t system_globals[] = {
	{ MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_system) },
	{ MP_ROM_QSTR(MP_QSTR_name), MP_ROM_PTR(&system_name_obj) },
	{ MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&system_info_obj) },
	{ MP_ROM_QSTR(MP_QSTR_reset_reason), MP_ROM_PTR(&system_reset_reason_obj) },
	{ MP_ROM_QSTR(MP_QSTR_set_stop_button), MP_ROM_PTR(&system_set_stop_button_obj) },
	{ MP_ROM_QSTR(MP_QSTR_shutdown), MP_ROM_PTR(&system_shutdown_obj) },
	{ MP_ROM_QSTR(MP_QSTR_storage), MP_ROM_PTR(&system_storage_obj) },
};
static MP_DEFINE_CONST_DICT(system_dict, system_globals);
const mp_obj_module_t python_system = {{&mp_type_module}, (mp_obj_dict_t *)&system_dict};
