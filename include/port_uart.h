// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <brickphyr_port.h>
#include <brickphyr_sensor.h>

/* Private measurement selector; public raw requests use bp_port_select(). */
#define PORT_RAW_MODE (1U << 31)
/* 16 modes, 14 INFO types, maximum messages at 2400 baud plus startup. */
#define PORT_UART_SYNC_TIMEOUT_MS (16 * 14 * 35 * 10 * 1000 / 2400 + 1000)

/* Immutable until the port worker completes or cancels it. */
struct port_uart_write {
	uint32_t request;
	uint8_t mode, size, data[32];
};

#ifdef CONFIG_BRICKPHYR_PORT_UART
/* Trusted port worker only. Start with the buffer disabled, then enable it
 * before the first poll. Stop IRQ access before reclaiming GPIO pins. */
int port_uart_start(uint32_t port);
int port_uart_serial_start(uint32_t port, uint32_t baud);
int port_uart_serial_poll(uint32_t port, const struct port_uart_write *write, uint32_t *completed);
/* Caller excludes reconfiguration; bounded copies, no application pointers retained. */
int port_uart_serial_read(uint32_t port, uint8_t *data, uint32_t size);
int port_uart_serial_waiting(uint32_t port);
int port_uart_poll(uint32_t port, struct bp_port_state *value,
	struct bp_sensor_sample *sensor, uint32_t measurement,
	const struct port_uart_write *write, bool cancel);
void port_uart_stop(uint32_t port);
bool port_uart_available(uint32_t port);
/* Concurrent snapshot reader: immutable metadata after negotiation. */
int port_uart_get_mode(uint32_t port, uint32_t mode, struct bp_port_mode *value);
#else
static inline bool port_uart_available(uint32_t port) { return false; }
static inline int port_uart_start(uint32_t port) { return -ENOTSUP; }
static inline int port_uart_serial_start(uint32_t port, uint32_t baud) { return -ENOTSUP; }
static inline int port_uart_serial_poll(uint32_t port, const struct port_uart_write *write,
	uint32_t *completed) { return -ENOTSUP; }
static inline int port_uart_serial_read(uint32_t port, uint8_t *data, uint32_t size) { return -ENOTSUP; }
static inline int port_uart_serial_waiting(uint32_t port) { return -ENOTSUP; }
static inline int port_uart_poll(uint32_t port, struct bp_port_state *value,
	struct bp_sensor_sample *sensor, uint32_t measurement,
	const struct port_uart_write *write, bool cancel) { return -ENOTSUP; }
static inline void port_uart_stop(uint32_t port) { }
static inline int port_uart_get_mode(uint32_t port, uint32_t mode,
	struct bp_port_mode *value) { return -ENOTSUP; }
#endif
