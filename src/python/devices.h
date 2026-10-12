// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "py/runtime.h"
#include <stdint.h>

mp_obj_t python_light_new(uint32_t light);
mp_obj_t python_matrix_new(void);
mp_obj_t python_speaker_new(void);
struct bp_port_state;
uint32_t python_port_connect(mp_obj_t port, uint16_t type, struct bp_port_state *value);
bool python_port_write_done(uint32_t port, uint32_t generation, uint32_t request);
extern const mp_obj_type_t python_pup_light_type, python_pup_matrix_type;
extern const mp_obj_type_t python_uart_type;
extern const mp_obj_type_t python_dc_motor_type, python_motor_type;
void python_motor_connection(mp_obj_t motor, uint32_t *port, uint32_t *generation);
extern const mp_obj_module_t python_system;
extern const mp_obj_base_t python_imu;
void python_imu_axes(mp_obj_t top, mp_obj_t front);

static inline void python_device_check(int result)
{
	if (result < 0) {
		mp_raise_OSError(-result);
	}
}
