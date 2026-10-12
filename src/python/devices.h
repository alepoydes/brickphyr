// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "py/runtime.h"
#include <stdint.h>

mp_obj_t python_light_new(uint32_t light);
mp_obj_t python_matrix_new(void);
mp_obj_t python_speaker_new(void);
struct bp_port_state;
uint32_t python_port_connect(mp_obj_t port, uint16_t type, struct bp_port_state *value);
extern const mp_obj_module_t python_system;
extern const mp_obj_base_t python_imu;
void python_imu_axes(mp_obj_t top, mp_obj_t front);

static inline void python_device_check(int result)
{
	if (result < 0) {
		mp_raise_OSError(-result);
	}
}
