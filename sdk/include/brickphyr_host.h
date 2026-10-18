// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

/* One native primary owns the host inbox. Payloads are application-defined
 * versioned binary values, not C structure images with implicit padding. */
#define BP_HOST_MAX_TYPES 8
#define BP_HOST_NAME_SIZE 16
#define BP_HOST_MAX_PAYLOAD 1024
#define BP_HOST_TOTAL_PAYLOAD 4096

enum bp_host_policy {
	BP_HOST_VALUE = 1,   /* A newer pending value replaces the older one. */
	BP_HOST_COMMAND = 2, /* Reject another command until completion. */
};

struct bp_host_type {
	uint16_t type, version, max_size;
	uint8_t policy;
	char name[BP_HOST_NAME_SIZE];
};

struct bp_host_message {
	uint32_t request;
	uint16_t size;
	int64_t received_us;
};

enum bp_host_state {
	BP_HOST_EMPTY,
	BP_HOST_PENDING,
	BP_HOST_PROCESSING,
	BP_HOST_COMPLETED,
	BP_HOST_SUPERSEDED,
};
