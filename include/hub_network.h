// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <brickphyr_network.h>

#ifdef CONFIG_BRICKPHYR_HUB_NETWORK
int hub_network_open(uint32_t size, uint32_t *handle);
int hub_network_get(uint32_t handle, struct bp_network_state *state);
int hub_network_connect(uint32_t handle, const uint8_t *address);
int hub_network_send(uint32_t handle, const struct bp_network_message *message);
int hub_network_cancel(uint32_t handle, uint32_t request);
int hub_network_snapshot(uint32_t handle, uint32_t latest, uint32_t *through);
int hub_network_read(uint32_t handle, uint32_t through, struct bp_network_message *message);
int hub_network_close(uint32_t handle);
int hub_network_release(void);
/* Radio-worker lifecycle; never called with an application-owned lock. */
int hub_network_start(void);
void hub_network_process(int error);
#else
static inline int hub_network_open(uint32_t n, uint32_t *h) { return -ENOTSUP; }
static inline int hub_network_get(uint32_t h, struct bp_network_state *s) { return -ENOTSUP; }
static inline int hub_network_connect(uint32_t h, const uint8_t *a) { return -ENOTSUP; }
static inline int hub_network_send(uint32_t h, const struct bp_network_message *m) { return -ENOTSUP; }
static inline int hub_network_cancel(uint32_t h, uint32_t r) { return -ENOTSUP; }
static inline int hub_network_snapshot(uint32_t h, uint32_t l, uint32_t *t) { return -ENOTSUP; }
static inline int hub_network_read(uint32_t h, uint32_t t, struct bp_network_message *m) { return -ENOTSUP; }
static inline int hub_network_close(uint32_t h) { return -ENOTSUP; }
static inline int hub_network_release(void) { return 0; }
static inline int hub_network_start(void) { return 0; }
static inline void hub_network_process(int error) {}
#endif
