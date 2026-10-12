// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <errno.h>
#include <brickphyr_imu.h>

#ifdef CONFIG_BRICKPHYR_IMU
/* Bounded copy. No I/O, allocation or configuration, including on first use. */
int hub_imu_get(struct bp_imu_sample *sample);
int hub_imu_get_state(struct bp_imu_state *state);
int hub_imu_configure(const struct bp_imu_command *command);
int hub_imu_get_application(struct bp_imu_application_state *state);
void hub_imu_release(void);
int hub_imu_shutdown(void);
#else
static inline int hub_imu_get(struct bp_imu_sample *sample) { return -ENOTSUP; }
static inline int hub_imu_get_state(struct bp_imu_state *state) { return -ENOTSUP; }
static inline int hub_imu_configure(const struct bp_imu_command *command) { return -ENOTSUP; }
static inline int hub_imu_get_application(struct bp_imu_application_state *state) { return -ENOTSUP; }
static inline void hub_imu_release(void) {}
static inline int hub_imu_shutdown(void) { return 0; }
#endif
