// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <brickphyr_port.h>

#ifdef CONFIG_BRICKPHYR_PORT_UART
/* Trusted port worker only. Start with the buffer disabled, then enable it
 * before the first poll. Stop IRQ access before reclaiming GPIO pins. */
int port_uart_start(uint32_t port);
int port_uart_poll(uint32_t port, struct bp_port_state *value);
void port_uart_stop(uint32_t port);
bool port_uart_available(uint32_t port);
#else
static inline bool port_uart_available(uint32_t port) { return false; }
static inline int port_uart_start(uint32_t port) { return -ENOTSUP; }
static inline int port_uart_poll(uint32_t port, struct bp_port_state *value) { return -ENOTSUP; }
static inline void port_uart_stop(uint32_t port) { }
#endif
