// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <brickphyr_port.h>
#include <brickphyr_sensor.h>
#include <brickphyr_motor.h>

#ifdef CONFIG_BRICKPHYR_PORTS
int hub_uart_open(enum bp_port_owner owner, uint32_t port, uint32_t baud, uint32_t *generation);
int hub_uart_configure(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t baud);
int hub_uart_read(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint8_t *data, uint32_t size);
int hub_uart_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const uint8_t *data, uint32_t size, uint32_t *request);
int hub_port_get(uint32_t port, struct bp_port_state *value);
int hub_port_get_mode(uint32_t port, uint32_t generation, uint32_t mode,
	struct bp_port_mode *value);
int hub_ports_shutdown(void);
int hub_port_select(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t mode);
int hub_port_write(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t mode,
	const uint8_t *data, uint32_t size, uint32_t *request);
int hub_port_reset(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t *request);
int hub_sensor_get(uint32_t port, struct bp_sensor_sample *value);
int hub_sensor_configure(enum bp_port_owner owner, uint32_t port, uint32_t measurement);
int hub_sensor_select(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t measurement);
int hub_sensor_palette_get(uint32_t port, uint32_t generation, struct bp_sensor_palette *palette);
int hub_sensor_palette_set(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const struct bp_sensor_palette *palette);
int hub_sensor_light_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const struct bp_sensor_light *light, uint32_t *request);
int hub_port_light_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	uint32_t brightness, uint32_t *request);
int hub_port_matrix_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const uint32_t *colors, uint32_t *request);
int hub_port_pf_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const struct bp_pf_command *command, uint32_t *request);
int hub_port_release(enum bp_port_owner owner, uint32_t port);
void hub_ports_session_closed(enum bp_port_owner owner);
int hub_ports_release(enum bp_port_owner owner);
int hub_motor_get(uint32_t port, struct bp_motor_state *value);
int hub_motor_setup(enum bp_port_owner owner, uint32_t port, uint32_t generation, int32_t direction, uint32_t *request);
int hub_motor_dc(enum bp_port_owner owner, uint32_t port, uint32_t generation, int32_t duty, uint32_t coast, uint32_t *request);
int hub_motor_set_voltage(enum bp_port_owner owner, uint32_t port, uint32_t generation, int32_t max_voltage);
int hub_motor_close(enum bp_port_owner owner, uint32_t port, uint32_t generation);
int hub_motor_observe(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t *request);
int hub_motor_observation_get(uint32_t port, uint32_t generation, uint32_t window_ms,
	struct bp_motor_observation *value);
int hub_motor_drive(enum bp_port_owner owner, uint32_t port, int32_t duty, uint32_t duration_ms);
int hub_motor_coast(enum bp_port_owner owner, uint32_t port);
int hub_motor_control(enum bp_port_owner owner, uint32_t port, const struct bp_motor_command *command);
/* Even means idle, odd active/unknown. Changes also expose short activity
 * between two IMU samples, so learning cannot span an unobserved drive. */
uint32_t hub_motor_activity(void);
#else
static inline int hub_uart_open(enum bp_port_owner owner, uint32_t port, uint32_t baud, uint32_t *generation) { return -ENOTSUP; }
static inline int hub_uart_configure(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t baud) { return -ENOTSUP; }
static inline int hub_uart_read(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint8_t *data, uint32_t size) { return -ENOTSUP; }
static inline int hub_uart_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const uint8_t *data, uint32_t size, uint32_t *request) { return -ENOTSUP; }
static inline int hub_port_get(uint32_t port, struct bp_port_state *value)
{
	return -ENOTSUP;
}
static inline int hub_ports_shutdown(void) { return 0; }
static inline int hub_port_select(enum bp_port_owner owner, uint32_t port,
	uint32_t generation, uint32_t mode) { return -ENOTSUP; }
static inline int hub_port_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	uint32_t mode, const uint8_t *data, uint32_t size, uint32_t *request) { return -ENOTSUP; }
static inline int hub_port_reset(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	uint32_t *request) { return -ENOTSUP; }
static inline int hub_port_get_mode(uint32_t port, uint32_t generation, uint32_t mode,
	struct bp_port_mode *value) { return -ENOTSUP; }
static inline int hub_sensor_get(uint32_t port, struct bp_sensor_sample *value) { return -ENOTSUP; }
static inline int hub_sensor_configure(enum bp_port_owner owner, uint32_t port,
	uint32_t measurement) { return -ENOTSUP; }
static inline int hub_sensor_select(enum bp_port_owner owner, uint32_t port,
	uint32_t generation, uint32_t measurement) { return -ENOTSUP; }
static inline int hub_sensor_palette_get(uint32_t port, uint32_t generation,
	struct bp_sensor_palette *palette) { return -ENOTSUP; }
static inline int hub_sensor_palette_set(enum bp_port_owner owner, uint32_t port,
	uint32_t generation, const struct bp_sensor_palette *palette) { return -ENOTSUP; }
static inline int hub_sensor_light_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const struct bp_sensor_light *light, uint32_t *request) { return -ENOTSUP; }
static inline int hub_port_light_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	uint32_t brightness, uint32_t *request) { return -ENOTSUP; }
static inline int hub_port_matrix_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const uint32_t *colors, uint32_t *request) { return -ENOTSUP; }
static inline int hub_port_pf_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const struct bp_pf_command *command, uint32_t *request) { return -ENOTSUP; }
static inline int hub_port_release(enum bp_port_owner owner, uint32_t port) { return -ENOTSUP; }
static inline void hub_ports_session_closed(enum bp_port_owner owner) { }
static inline int hub_ports_release(enum bp_port_owner owner) { return 0; }
static inline int hub_motor_get(uint32_t port, struct bp_motor_state *value) { return -ENOTSUP; }
static inline int hub_motor_setup(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	int32_t direction, uint32_t *request) { return -ENOTSUP; }
static inline int hub_motor_dc(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	int32_t duty, uint32_t coast, uint32_t *request) { return -ENOTSUP; }
static inline int hub_motor_set_voltage(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	int32_t max_voltage) { return -ENOTSUP; }
static inline int hub_motor_close(enum bp_port_owner owner, uint32_t port, uint32_t generation) { return -ENOTSUP; }
static inline int hub_motor_observe(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	uint32_t *request) { return -ENOTSUP; }
static inline int hub_motor_observation_get(uint32_t port, uint32_t generation, uint32_t window_ms,
	struct bp_motor_observation *value) { return -ENOTSUP; }
static inline int hub_motor_drive(enum bp_port_owner owner, uint32_t port, int32_t duty,
	uint32_t duration_ms) { return -ENOTSUP; }
static inline int hub_motor_coast(enum bp_port_owner owner, uint32_t port) { return -ENOTSUP; }
static inline int hub_motor_control(enum bp_port_owner owner, uint32_t port,
	const struct bp_motor_command *command) { return -ENOTSUP; }
static inline uint32_t hub_motor_activity(void) { return 0; }
#endif
