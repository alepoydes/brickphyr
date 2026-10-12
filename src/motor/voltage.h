// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2023 LEGO System A/S
/* Model limit from lib/pbio/src/motor/servo_settings.c at a7b1b04ae. */
#pragma once
#include <stdint.h>
#include <lego/device.h>

static inline int32_t motor_max_voltage(uint16_t id)
{
	return id == LEGO_DEVICE_TYPE_ID_SPIKE_S_MOTOR ? 6000 : 9000;
}
