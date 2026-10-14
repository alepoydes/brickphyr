// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
// Adapted from pybricks/iodevices/pb_type_iodevices_pupdevice.c.
#include "devices.h"
#include "py/mphal.h"
#include <brickphyr.h>
#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <lego/device.h>
#include <lego/lump.h>
#include <pybricks/parameters.h>
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_type_enum.h>

#ifdef CONFIG_BRICKPHYR_PORTS
typedef struct {
	mp_obj_base_t base;
	pb_type_async_t *iter;
	uint32_t port, generation;
#ifdef CONFIG_BRICKPHYR_PORT_UART
	uint32_t write_request, reset_request;
#endif
	uint16_t id;
#ifdef CONFIG_BRICKPHYR_PORT_UART
	uint8_t mode;
#endif
	bool passive;
} pup_obj_t;

#ifdef CONFIG_BRICKPHYR_PORT_UART
/* Only this object's explicit reset permits accepting a new generation. A
 * later reconnection cannot impersonate the recorded completion. */
static void resolve_reset(pup_obj_t *self)
{
	if (!self->reset_request) return;
	struct bp_port_state value;
	python_device_check(bp_port_get(self->port, &value));
	if (value.reset.request != self->reset_request) python_device_check(-ECANCELED);
	if (value.reset.completed != self->reset_request) python_device_check(-EAGAIN);
	python_device_check(value.reset.error);
	if (value.reset.generation != value.generation || value.type_id != self->id) {
		python_device_check(-ENODEV);
	}
	self->generation = value.generation;
	self->reset_request = 0;
}
#endif

static void get_port(pup_obj_t *self, struct bp_port_state *value)
{
	python_device_check(bp_port_get(self->port, value));
	if (value->generation != self->generation) python_device_check(-ENODEV);
	python_device_check(value->error);
}

uint32_t python_port_connect(mp_obj_t port_in, uint16_t type, struct bp_port_state *value)
{
	pb_module_tools_assert_blocking();
	int port = pb_type_enum_get_value(port_in, &pb_enum_type_Port) - PBIO_PORT_ID_A;
	if (port < 0 || port >= BP_PORT_COUNT) mp_raise_ValueError(MP_ERROR_TEXT("expected port A-F"));
	for (;;) {
		python_device_check(bp_port_get(port, value));
		if (value->phase == BP_PORT_READY || value->phase == BP_PORT_PASSIVE) break;
		if (value->phase == BP_PORT_EMPTY) python_device_check(-ENODEV);
		if (value->error != -EAGAIN) python_device_check(value->error);
		mp_hal_delay_ms(2);
	}
	/* Negotiated metadata is usable even before the first DATA frame. */
	if (value->error != -EAGAIN) python_device_check(value->error);
	if (type && value->type_id != type) python_device_check(-ENODEV);
	return port;
}

static mp_obj_t pup_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	PB_PARSE_ARGS_CLASS(n_args, n_kw, args, PB_ARG_REQUIRED(port));
	struct bp_port_state value;
	uint32_t port = python_port_connect(port_in, 0, &value);
	pup_obj_t *self = mp_obj_malloc(pup_obj_t, type);
	*self = (pup_obj_t){.base = {.type = type}, .port = port,
		.generation = value.generation, .id = value.type_id,
		.passive = value.phase == BP_PORT_PASSIVE};
	return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t pup_info(mp_obj_t self_in)
{
	pup_obj_t *self = MP_OBJ_TO_PTR(self_in);
#ifdef CONFIG_BRICKPHYR_PORT_UART
	resolve_reset(self);
#endif
	struct bp_port_state value;
	python_device_check(bp_port_get(self->port, &value));
	if (value.generation != self->generation) python_device_check(-ENODEV);
	if (value.error != -EAGAIN) python_device_check(value.error);
	mp_obj_t result = mp_obj_new_dict(2);
	mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_id), mp_obj_new_int(self->id));
#ifdef CONFIG_BRICKPHYR_PORT_UART
	if (!self->passive) {
		mp_obj_t modes[LUMP_MAX_EXT_MODE + 1];
		for (uint32_t mode = 0; mode < value.num_modes; mode++) {
			struct bp_port_mode info;
			int err = bp_port_get_mode(self->port, self->generation, mode, &info);
			python_device_check(err == -ESTALE ? -ENODEV : err);
			mp_obj_t items[] = {mp_obj_new_str(info.name, strlen(info.name)),
				mp_obj_new_int(info.num_values), mp_obj_new_int(info.data_type)};
			modes[mode] = mp_obj_new_tuple(MP_ARRAY_SIZE(items), items);
		}
		mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_modes), mp_obj_new_tuple(value.num_modes, modes));
		python_device_check(bp_port_get(self->port, &value));
		if (value.generation != self->generation) python_device_check(-ENODEV);
	}
#endif
	return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(pup_info_obj, pup_info);

#ifdef CONFIG_BRICKPHYR_PORT_UART
static pbio_error_t read_ready(pbio_os_state_t *state, mp_obj_t self_in)
{
	pup_obj_t *self = MP_OBJ_TO_PTR(self_in);
	struct bp_port_state value;
	python_device_check(bp_port_get(self->port, &value));
	if (value.generation != self->generation) python_device_check(-ENODEV);
	if (value.error != -EAGAIN) python_device_check(value.error);
	if (value.selected_mode != self->mode) python_device_check(-ECANCELED);
	if (value.mode_error == -EAGAIN) return PBIO_ERROR_AGAIN;
	python_device_check(value.mode_error);
	return value.mode == self->mode && value.length ? PBIO_SUCCESS : PBIO_ERROR_AGAIN;
}

static mp_obj_t read_values(mp_obj_t self_in)
{
	pup_obj_t *self = MP_OBJ_TO_PTR(self_in);
	struct bp_port_state value;
	get_port(self, &value);
	python_device_check(value.mode_error);
	if (value.selected_mode != self->mode || value.mode != self->mode) python_device_check(-ECANCELED);
	mp_obj_t values[LUMP_MAX_MSG_SIZE];
	for (uint8_t i = 0; i < value.num_values; i++) {
		switch (value.data_type) {
		case LUMP_DATA_TYPE_DATA8:
			values[i] = mp_obj_new_int((int8_t)value.data[i]);
			break;
		case LUMP_DATA_TYPE_DATA16:
			values[i] = mp_obj_new_int((int16_t)sys_get_le16(value.data + 2 * i));
			break;
		case LUMP_DATA_TYPE_DATA32:
			values[i] = mp_obj_new_int((int32_t)sys_get_le32(value.data + 4 * i));
			break;
		case LUMP_DATA_TYPE_DATAF: {
			uint32_t bits = sys_get_le32(value.data + 4 * i);
			float number;
			memcpy(&number, &bits, sizeof(number));
			values[i] = mp_obj_new_float_from_f(number);
			break;
		}
		default: python_device_check(-EPROTO);
		}
	}
	return mp_obj_new_tuple(value.num_values, values);
}
#endif

static mp_obj_t pup_read(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, pup_obj_t, self, PB_ARG_REQUIRED(mode));
#ifdef CONFIG_BRICKPHYR_PORT_UART
	if (self->iter && self->iter->parent_obj != MP_OBJ_NULL) python_device_check(-EBUSY);
	resolve_reset(self);
#endif
	if (self->passive) {
		if (self->id != LEGO_DEVICE_TYPE_ID_LPF2_TOUCH) python_device_check(-ENOTSUP);
		struct bp_port_state value;
		get_port(self, &value);
		return pb_type_async_return_result(mp_obj_new_bool(value.pressed), &self->iter);
	}
#ifdef CONFIG_BRICKPHYR_PORT_UART
	mp_int_t mode = mp_obj_get_int(mode_in);
	if (mode < 0 || mode > LUMP_MAX_EXT_MODE) mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
	int err = bp_port_select(self->port, self->generation, mode);
	if (err == -EINVAL) mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
	python_device_check(err == -ESTALE ? -ENODEV : err);
	self->mode = mode;
	pb_type_async_t config = {.parent_obj = MP_OBJ_FROM_PTR(self),
		.iter_once = read_ready, .return_map = read_values};
	if (pb_module_tools_run_loop_is_active()) {
		return pb_type_async_wait_or_await(&config, &self->iter, false);
	}
	/* The port worker advances at >=2 ms intervals. Use the VM's timed wait
	 * instead of depending on PBIO's absent cooperative event scheduler. */
	while (read_ready(&config.state, config.parent_obj) == PBIO_ERROR_AGAIN) mp_hal_delay_ms(2);
	return read_values(config.parent_obj);
#else
	(void)mode_in;
	python_device_check(-ENOTSUP);
	return mp_const_none;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pup_read_obj, 1, pup_read);

#ifdef CONFIG_BRICKPHYR_PORT_UART
bool python_port_write_done(uint32_t port, uint32_t generation, uint32_t request)
{
	struct bp_port_state value;
	python_device_check(bp_port_get(port, &value));
	if (value.generation != generation) python_device_check(-ENODEV);
	if (value.error != -EAGAIN) python_device_check(value.error);
	if (value.write.request != request) python_device_check(-ECANCELED);
	if (value.write.completed != request) return false;
	python_device_check(value.write.error);
	return true;
}

static pbio_error_t write_ready(pbio_os_state_t *state, mp_obj_t self_in)
{
	pup_obj_t *self = MP_OBJ_TO_PTR(self_in);
	return python_port_write_done(self->port, self->generation, self->write_request) ? PBIO_SUCCESS : PBIO_ERROR_AGAIN;
}

static mp_obj_t pup_write(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, args, kw_args, pup_obj_t, self,
		PB_ARG_REQUIRED(mode), PB_ARG_REQUIRED(data));
	if (self->iter && self->iter->parent_obj != MP_OBJ_NULL) python_device_check(-EBUSY);
	resolve_reset(self);
	if (self->passive) python_device_check(-ENOTSUP);
	mp_int_t mode = mp_obj_get_int(mode_in);
	if (mode < 0 || mode > LUMP_MAX_EXT_MODE) mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
	struct bp_port_mode info;
	int err = bp_port_get_mode(self->port, self->generation, mode, &info);
	if (err == -EINVAL) mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
	python_device_check(err == -ESTALE ? -ENODEV : err);
	if (!info.output_mapping) mp_raise_ValueError(MP_ERROR_TEXT("mode not writable"));
	size_t count;
	mp_obj_t *values;
	mp_obj_get_array(data_in, &count, &values);
	if (count != info.num_values) mp_raise_ValueError(MP_ERROR_TEXT("wrong value count"));
	uint8_t data[LUMP_MAX_MSG_SIZE];
	uint32_t width = info.data_type == LUMP_DATA_TYPE_DATA8 ? 1 :
		info.data_type == LUMP_DATA_TYPE_DATA16 ? 2 : 4;
	for (size_t i = 0; i < count; i++) {
		if (info.data_type == LUMP_DATA_TYPE_DATAF) {
			float number = mp_obj_get_float_to_f(values[i]);
			uint32_t bits;
			memcpy(&bits, &number, sizeof(bits));
			sys_put_le32(bits, data + 4 * i);
			continue;
		}
		mp_int_t value = mp_obj_get_int(values[i]);
		if ((width == 1 && (value < INT8_MIN || value > INT8_MAX)) ||
		    (width == 2 && (value < INT16_MIN || value > INT16_MAX)) ||
		    (width == 4 && (value < INT32_MIN || value > INT32_MAX))) {
			mp_raise_ValueError(MP_ERROR_TEXT("value out of range"));
		}
		if (width == 1) data[i] = value;
		else if (width == 2) sys_put_le16(value, data + 2 * i);
		else sys_put_le32(value, data + 4 * i);
	}
	err = bp_port_write(self->port, self->generation, mode, data, count * width, &self->write_request);
	python_device_check(err == -ESTALE ? -ENODEV : err);
	pb_type_async_t config = {.parent_obj = MP_OBJ_FROM_PTR(self), .iter_once = write_ready};
	if (pb_module_tools_run_loop_is_active()) {
		return pb_type_async_wait_or_await(&config, &self->iter, false);
	}
	while (write_ready(&config.state, config.parent_obj) == PBIO_ERROR_AGAIN) mp_hal_delay_ms(2);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pup_write_obj, 1, pup_write);

static mp_obj_t pup_reset(mp_obj_t self_in)
{
	pup_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (self->iter && self->iter->parent_obj != MP_OBJ_NULL) python_device_check(-EBUSY);
	if (self->passive) python_device_check(-ENOTSUP);
	resolve_reset(self);
	int err = bp_port_reset(self->port, self->generation, &self->reset_request);
	python_device_check(err == -ESTALE ? -ENODEV : err);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(pup_reset_obj, pup_reset);
#endif

static const mp_rom_map_elem_t pup_methods[] = {
	{MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&pup_info_obj)},
	{MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&pup_read_obj)},
#ifdef CONFIG_BRICKPHYR_PORT_UART
	{MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&pup_write_obj)},
	{MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&pup_reset_obj)},
#endif
};
static MP_DEFINE_CONST_DICT(pup_dict, pup_methods);
static MP_DEFINE_CONST_OBJ_TYPE(pup_type, MP_QSTR_PUPDevice, MP_TYPE_FLAG_NONE,
	make_new, pup_new, locals_dict, &pup_dict);
#endif

static const mp_rom_map_elem_t iodevices_globals[] = {
	{MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_iodevices)},
#ifdef CONFIG_BRICKPHYR_PORTS
	{MP_ROM_QSTR(MP_QSTR_PUPDevice), MP_ROM_PTR(&pup_type)},
	{MP_ROM_QSTR(MP_QSTR_LUMPDevice), MP_ROM_PTR(&pup_type)},
#endif
#ifdef CONFIG_BRICKPHYR_PORT_UART
	{MP_ROM_QSTR(MP_QSTR_UARTDevice), MP_ROM_PTR(&python_uart_type)},
#endif
#ifdef CONFIG_BRICKPHYR_BLE_PEER
	{MP_ROM_QSTR(MP_QSTR_LWP3Device), MP_ROM_PTR(&python_lwp3device_type)},
	{MP_ROM_QSTR(MP_QSTR_XboxController), MP_ROM_PTR(&python_xbox_type)},
#endif
#ifdef CONFIG_BRICKPHYR_MOTORS
	{MP_ROM_QSTR(MP_QSTR_DCMotor), MP_ROM_PTR(&python_dc_motor_type)},
#endif
};
static MP_DEFINE_CONST_DICT(iodevices_dict, iodevices_globals);
const mp_obj_module_t pb_module_iodevices = {{&mp_type_module}, (mp_obj_dict_t *)&iodevices_dict};
