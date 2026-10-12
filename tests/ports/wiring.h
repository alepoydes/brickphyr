// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Electrical circuits, not scripted replies to detector states. */
enum pup_test_kind { UNPLUGGED, MEDIUM_MOTOR, TRAIN_MOTOR, LIGHT, UART_DEVICE, TOUCH_SWITCH };
struct pup_test_circuit {
	enum pup_test_kind kind;
	bool pressed;
	int read_error, release_error, coast_error, supply_error;
	uint32_t reads, writes, supply_changes;
};
extern struct pup_test_circuit pup_test_circuits[6];
bool pup_test_supply(void);
uint8_t pup_test_sensor_supply(uint32_t port);
bool pup_test_released(uint32_t port);
