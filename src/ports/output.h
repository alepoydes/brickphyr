// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2022 The Pybricks Authors
#pragma once
#include <errno.h>
#include <stdint.h>

/* Trusted port service only, serialized with detection and sensor supply.
 * No application receives GPIO/PWM handles. On an I/O failure the caller
 * must attempt coast and retain that cleanup error separately. */
int port_output_coast(uint32_t port);
int port_output_supply(uint32_t port, uint8_t pin);
#ifdef CONFIG_BRICKPHYR_MOTORS
/* Availability/readiness only, no peripheral operations. */
int port_output_check(uint32_t port);
/* Pybricks direction, -1000..1000 permille; zero brakes, not coast. */
int port_output_drive(uint32_t port, int32_t duty);
#else
static inline int port_output_check(uint32_t port) { return -ENOTSUP; }
static inline int port_output_drive(uint32_t port, int32_t duty) { return -ENOTSUP; }
#endif
