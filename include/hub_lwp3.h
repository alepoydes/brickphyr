// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <brickphyr_lwp3.h>

#ifdef CONFIG_BRICKPHYR_LWP3
#include <zephyr/bluetooth/addr.h>
int hub_lwp3_open(const struct bp_lwp3_config *config, uint32_t *handle);
int hub_lwp3_connect(uint32_t handle);
int hub_lwp3_disconnect(uint32_t handle);
int hub_lwp3_get(uint32_t handle, struct bp_lwp3_state *state);
int hub_lwp3_write(uint32_t handle, const void *data, size_t size);
int hub_lwp3_read(uint32_t handle, struct bp_lwp3_message *message);
int hub_lwp3_close(uint32_t handle);
int hub_lwp3_command(uint32_t handle, uint32_t command, int32_t value, int32_t auxiliary);
int hub_lwp3_palette_get(uint32_t handle, struct bp_sensor_palette *palette);
int hub_lwp3_palette_set(uint32_t handle, const struct bp_sensor_palette *palette);
int hub_lwp3_release(void);
/* Radio-worker integration. 0: ordinary passive observation, 1: active scan,
 * 2: pause explicit scanning while initiating a connection. */
int hub_lwp3_scan_mode(void);
void hub_lwp3_process(int radio_error, bool scan_stopped);
void hub_lwp3_receive(const bt_addr_le_t *addr, uint8_t type, const uint8_t *data, size_t size);
#else
static inline int hub_lwp3_open(const struct bp_lwp3_config *c, uint32_t *h) { return -ENOTSUP; }
static inline int hub_lwp3_connect(uint32_t h) { return -ENOTSUP; }
static inline int hub_lwp3_disconnect(uint32_t h) { return -ENOTSUP; }
static inline int hub_lwp3_get(uint32_t h, struct bp_lwp3_state *s) { return -ENOTSUP; }
static inline int hub_lwp3_write(uint32_t h, const void *d, size_t n) { return -ENOTSUP; }
static inline int hub_lwp3_read(uint32_t h, struct bp_lwp3_message *m) { return -ENOTSUP; }
static inline int hub_lwp3_close(uint32_t h) { return -ENOTSUP; }
static inline int hub_lwp3_command(uint32_t h, uint32_t c, int32_t v, int32_t a) { return -ENOTSUP; }
static inline int hub_lwp3_palette_get(uint32_t h, struct bp_sensor_palette *p) { return -ENOTSUP; }
static inline int hub_lwp3_palette_set(uint32_t h, const struct bp_sensor_palette *p) { return -ENOTSUP; }
static inline int hub_lwp3_release(void) { return 0; }
static inline int hub_lwp3_scan_mode(void) { return 0; }
static inline void hub_lwp3_process(int error, bool scan_stopped) {}
#endif
