// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2022 The Pybricks Authors
#pragma once
#include <errno.h>
#include <stdint.h>
#include <brickphyr_control.h>

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
/* One prepared group enters the output adapter once. A backend reports only
 * writes which reached its outputs, including writes preceding a failure.
 * check() revalidates ownership/cancellation immediately before application;
 * a backend with a shared latch calls it once before that latch is armed.
 * An all-coast cleanup attempts every member even if an earlier one fails.
 * The caller still owns cleanup and first-error preservation. */
struct port_output_result {
	uint32_t applied_mask;
	int errors[BP_PORT_COUNT];
	int64_t applied_us[BP_PORT_COUNT];
};
int port_output_group(const struct bp_motor_group_command *command,
	struct port_output_result *result, int (*check)(void));
#else
static inline int port_output_check(uint32_t port) { return -ENOTSUP; }
static inline int port_output_drive(uint32_t port, int32_t duty) { return -ENOTSUP; }
#endif
