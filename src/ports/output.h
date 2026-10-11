// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2022 The Pybricks Authors
#pragma once
#include <stdint.h>

/* Trusted port service only, serialized with detection and sensor supply.
 * No application receives GPIO/PWM handles. On an I/O failure the caller
 * must attempt coast and retain that cleanup error separately. */
int port_output_coast(uint32_t port);
int port_output_supply(uint32_t port, uint8_t pin);
/* Pybricks direction, -1000..1000 permille; zero brakes, not coast. */
int port_output_drive(uint32_t port, int32_t duty);
