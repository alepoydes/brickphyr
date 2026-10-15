// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Presentation of little-endian 32-bit Settings values. The owning Settings
 * handler alone validates and applies them; this is not another value store. */
struct config_entry {
	const char *name;
	const char *unit;
	size_t count;
	bool is_signed;
};

#define CONFIG_VALUE_BYTES (6 * sizeof(uint32_t))
extern const struct config_entry power_config[2];
extern const struct config_entry imu_config[6];

int config_index(const struct config_entry *entries, size_t count, const char *name);
int config_load(const struct config_entry *entries, size_t count);
int config_export(const struct config_entry *entries, size_t count,
	int (*save)(const char *, const void *, size_t));
/* Trusted synchronous save. Application callers use the queued operation. */
int config_save(void);
int config_save_start(void);
int config_save_status(uint32_t request);
/* Shutdown waits only for an explicitly requested save; it does not start one. */
int config_save_finish(void);
