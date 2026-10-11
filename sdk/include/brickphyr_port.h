// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

enum bp_port_owner { BP_PORT_UNOWNED, BP_PORT_APPLICATION, BP_PORT_TERMINAL };

/* UART is the electrical signature; NEGOTIATING and READY describe LUMP. */
enum bp_port_phase {
	BP_PORT_DETECTING,
	BP_PORT_EMPTY,
	BP_PORT_PASSIVE,
	BP_PORT_UART,
	BP_PORT_FAULT,
	BP_PORT_OFF,
	BP_PORT_NEGOTIATING,
	BP_PORT_READY,
};

#define BP_PORT_COUNT 6

struct bp_port_state {
	/* Service update time and last phase/identity change, monotonic ms.
	 * A conflicting scan invalidates identity during the new debounce. */
	int64_t sample_ms, changed_ms;
	/* Sequence and generation wrap; the failed-scan counter saturates. */
	uint32_t sequence, generation, failures;
	/* Consult error before using identity. Cleanup never replaces it. */
	int32_t error, cleanup_error;
	uint16_t type_id; /* LEGO device ID; 14 is unidentified UART. */
	uint8_t phase;
	uint8_t pressed; /* Only meaningful for passive touch (type 5). */
	/* Last valid UART DATA payload, in the device's wire units. This is not
	 * a calibrated sensor API. Zero length means no current sample. data_ms
	 * is parser completion time, not a timestamp supplied by the sensor.
	 * DATA sequence and bad-frame count restart on each UART connection. */
	int64_t data_ms;
	uint32_t data_sequence, baudrate, bad_frames;
	uint8_t mode, num_modes, capabilities, length;
	uint8_t data_type, num_values;
	uint8_t supply; /* Sensor DC: 0 none, 1 P1, 2 P2. Motor drive is reported separately. */
	uint8_t data[32];
};
