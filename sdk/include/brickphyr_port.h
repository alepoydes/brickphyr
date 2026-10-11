// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

/* Electrical identification only. UART means that negotiation is still
 * required, not that a sensor can already provide measurements. */
enum bp_port_phase {
	BP_PORT_DETECTING,
	BP_PORT_EMPTY,
	BP_PORT_PASSIVE,
	BP_PORT_UART,
	BP_PORT_FAULT,
	BP_PORT_OFF,
};

#define BP_PORT_COUNT 6

struct bp_port_state {
	/* Completion/failure time and last phase/identity change, monotonic ms.
	 * A conflicting scan invalidates identity during the new debounce. */
	int64_t sample_ms, changed_ms;
	/* Sequence and generation wrap; the failed-scan counter saturates. */
	uint32_t sequence, generation, failures;
	/* Consult error before using identity. Cleanup never replaces it. */
	int32_t error, cleanup_error;
	uint16_t type_id; /* LEGO electrical device ID; 14 is unidentified UART. */
	uint8_t phase;
	uint8_t pressed; /* Only meaningful for passive touch (type 5). */
};
