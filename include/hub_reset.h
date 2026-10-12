// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <stdint.h>

/* Cached boot cause; reading it never clears hardware flags. */
#ifdef CONFIG_HWINFO
int hub_reset_cause(uint32_t *cause);
#else
static inline int hub_reset_cause(uint32_t *cause) { return -ENOTSUP; }
#endif
