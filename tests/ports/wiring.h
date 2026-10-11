// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Electrical circuits, not scripted replies to detector states. */
enum pup_test_kind { UNPLUGGED, MEDIUM_MOTOR, TRAIN_MOTOR, LIGHT, UART_DEVICE, TOUCH_SWITCH };
struct pup_test_circuit {
	enum pup_test_kind kind;
	bool pressed;
	int read_error, release_error;
	uint32_t reads, writes;
};
extern struct pup_test_circuit pup_test_circuits[6];
bool pup_test_supply(void);
bool pup_test_released(uint32_t port);
