// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <errno.h>
#include <brickphyr_radio.h>

#ifdef CONFIG_BRICKPHYR_RADIO
int hub_radio_observe(const uint8_t *channels, size_t count, uint32_t *handle);
int hub_radio_ready(uint32_t handle);
int hub_radio_version(char *value, size_t size);
int hub_radio_get(uint32_t handle, uint32_t channel, struct bp_radio_sample *sample);
int hub_radio_close(uint32_t handle);
int hub_radio_release(void);
#else
static inline int hub_radio_observe(const uint8_t *channels, size_t count, uint32_t *handle) { return -ENOTSUP; }
static inline int hub_radio_ready(uint32_t handle) { return -ENOTSUP; }
static inline int hub_radio_version(char *value, size_t size) { return -ENOTSUP; }
static inline int hub_radio_get(uint32_t handle, uint32_t channel, struct bp_radio_sample *sample) { return -ENOTSUP; }
static inline int hub_radio_close(uint32_t handle) { return -ENOTSUP; }
static inline int hub_radio_release(void) { return 0; }
#endif
