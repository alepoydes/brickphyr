// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <brickphyr_port.h>
#include <brickphyr_sensor.h>
#include <brickphyr_motor.h>

#ifdef CONFIG_BRICKPHYR_PORTS
int hub_port_get(uint32_t port, struct bp_port_state *value);
int hub_port_get_mode(uint32_t port, uint32_t generation, uint32_t mode,
	struct bp_port_mode *value);
int hub_ports_shutdown(void);
int hub_sensor_get(uint32_t port, struct bp_sensor_sample *value);
int hub_sensor_configure(enum bp_port_owner owner, uint32_t port, uint32_t measurement);
int hub_port_release(enum bp_port_owner owner, uint32_t port);
void hub_ports_session_closed(enum bp_port_owner owner);
int hub_ports_release(enum bp_port_owner owner);
int hub_motor_get(uint32_t port, struct bp_motor_state *value);
int hub_motor_drive(enum bp_port_owner owner, uint32_t port, int32_t duty, uint32_t duration_ms);
int hub_motor_coast(enum bp_port_owner owner, uint32_t port);
/* Even means idle, odd active/unknown. Changes also expose short activity
 * between two IMU samples, so learning cannot span an unobserved drive. */
uint32_t hub_motor_activity(void);
#else
static inline int hub_port_get(uint32_t port, struct bp_port_state *value)
{
	return -ENOTSUP;
}
static inline int hub_ports_shutdown(void) { return 0; }
static inline int hub_port_get_mode(uint32_t port, uint32_t generation, uint32_t mode,
	struct bp_port_mode *value) { return -ENOTSUP; }
static inline int hub_sensor_get(uint32_t port, struct bp_sensor_sample *value) { return -ENOTSUP; }
static inline int hub_sensor_configure(enum bp_port_owner owner, uint32_t port,
	uint32_t measurement) { return -ENOTSUP; }
static inline int hub_port_release(enum bp_port_owner owner, uint32_t port) { return -ENOTSUP; }
static inline void hub_ports_session_closed(enum bp_port_owner owner) { }
static inline int hub_ports_release(enum bp_port_owner owner) { return 0; }
static inline int hub_motor_get(uint32_t port, struct bp_motor_state *value) { return -ENOTSUP; }
static inline int hub_motor_drive(enum bp_port_owner owner, uint32_t port, int32_t duty,
	uint32_t duration_ms) { return -ENOTSUP; }
static inline int hub_motor_coast(enum bp_port_owner owner, uint32_t port) { return -ENOTSUP; }
static inline uint32_t hub_motor_activity(void) { return 0; }
#endif
