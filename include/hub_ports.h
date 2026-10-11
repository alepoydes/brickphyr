// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <brickphyr_port.h>

#ifdef CONFIG_BRICKPHYR_PORTS
int hub_port_get(uint32_t port, struct bp_port_state *value);
int hub_ports_shutdown(void);
#else
static inline int hub_port_get(uint32_t port, struct bp_port_state *value)
{
	return -ENOTSUP;
}
static inline int hub_ports_shutdown(void) { return 0; }
#endif
