// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <brickphyr_port.h>
#include <brickphyr_sensor.h>

#ifdef CONFIG_BRICKPHYR_PORTS
int hub_port_get(uint32_t port, struct bp_port_state *value);
int hub_ports_shutdown(void);
int hub_sensor_get(uint32_t port, struct bp_sensor_sample *value);
int hub_sensor_configure(enum bp_sensor_owner owner, uint32_t port, uint32_t measurement);
int hub_sensor_release(enum bp_sensor_owner owner, uint32_t port);
void hub_sensors_release(enum bp_sensor_owner owner);
#else
static inline int hub_port_get(uint32_t port, struct bp_port_state *value)
{
	return -ENOTSUP;
}
static inline int hub_ports_shutdown(void) { return 0; }
static inline int hub_sensor_get(uint32_t port, struct bp_sensor_sample *value) { return -ENOTSUP; }
static inline int hub_sensor_configure(enum bp_sensor_owner owner, uint32_t port,
	uint32_t measurement) { return -ENOTSUP; }
static inline int hub_sensor_release(enum bp_sensor_owner owner, uint32_t port) { return -ENOTSUP; }
static inline void hub_sensors_release(enum bp_sensor_owner owner) { }
#endif
