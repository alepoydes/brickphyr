// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "py/runtime.h"
#include <stdint.h>

mp_obj_t python_light_new(uint32_t light);
mp_obj_t python_matrix_new(void);
mp_obj_t python_speaker_new(void);
extern const mp_obj_module_t python_system;
extern const mp_obj_base_t python_imu;

static inline void python_device_check(int result)
{
	if (result < 0) {
		mp_raise_OSError(-result);
	}
}
