// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <brickphyr_port.h>
#include <brickphyr_sensor.h>

/* Private measurement selector; public raw requests use bp_port_select(). */
#define PORT_RAW_MODE (1U << 31)

#ifdef CONFIG_BRICKPHYR_PORT_UART
/* Trusted port worker only. Start with the buffer disabled, then enable it
 * before the first poll. Stop IRQ access before reclaiming GPIO pins. */
int port_uart_start(uint32_t port);
int port_uart_poll(uint32_t port, struct bp_port_state *value,
	struct bp_sensor_sample *sensor, uint32_t measurement);
void port_uart_stop(uint32_t port);
bool port_uart_available(uint32_t port);
/* Concurrent snapshot reader: immutable metadata after negotiation. */
int port_uart_get_mode(uint32_t port, uint32_t mode, struct bp_port_mode *value);
#else
static inline bool port_uart_available(uint32_t port) { return false; }
static inline int port_uart_start(uint32_t port) { return -ENOTSUP; }
static inline int port_uart_poll(uint32_t port, struct bp_port_state *value,
	struct bp_sensor_sample *sensor, uint32_t measurement) { return -ENOTSUP; }
static inline void port_uart_stop(uint32_t port) { }
static inline int port_uart_get_mode(uint32_t port, uint32_t mode,
	struct bp_port_mode *value) { return -ENOTSUP; }
#endif
