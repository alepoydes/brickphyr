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

enum bp_port_mode_fields {
	BP_PORT_MODE_FLAGS = 1 << 0,
	BP_PORT_MODE_RAW = 1 << 1,
	BP_PORT_MODE_PERCENT = 1 << 2,
	BP_PORT_MODE_SI = 1 << 3,
	BP_PORT_MODE_UNIT = 1 << 4,
	BP_PORT_MODE_MAPPING = 1 << 5,
	BP_PORT_MODE_COMBINATIONS = 1 << 6,
};

/* Advertised metadata, not calibrated measurements or permission to actuate.
 * Optional fields require their present bit; NAME and FORMAT are mandatory.
 * float fields retain the device's IEEE-754 values without applying scaling. */
struct bp_port_mode {
	float raw[2], percent[2], si[2];
	uint16_t combinations[16];
	/* Protocol INFO 0x07..0x0c, unchanged wire payloads including padding.
	 * Zero length means absent; their device-specific meaning is not inferred. */
	uint8_t extra_length[6], extra[6][32];
	uint16_t other_info; /* Saturating count of undefined INFO types not retained. */
	char name[12], unit[5];
	uint8_t flags[6];
	uint8_t present, num_values, data_type, digits, decimals;
	uint8_t input_mapping, output_mapping, num_combinations;
};

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
	uint32_t firmware_version, hardware_version; /* Advertised BCD words. */
	uint8_t has_version, num_view_modes;
	uint8_t mode, num_modes, capabilities, length;
	uint8_t data_type, num_values;
	uint8_t supply; /* Sensor DC: 0 none, 1 P1, 2 P2. Motor drive is reported separately. */
	uint8_t data[32];
	/* Explicit raw-mode selection: UINT8_MAX when normalized sensing owns
	 * the mode. Check error, mode_error and matching mode before using data.
	 * Settling and fresh-sample checks belong to the shared port worker. */
	int32_t mode_error;
	uint8_t selected_mode;
	/* A write completes only after UART transmission and device settling.
	 * Match request and generation; a later write replaces the result.
	 * Request != completed means pending; error is then -EAGAIN. */
	struct {
		uint32_t request, completed;
		int32_t error;
	} write;
	/* Explicit resynchronization. Completion identifies the newly negotiated
	 * generation; later hotplug never updates that result. No power-cycle
	 * guarantee. Request != completed means pending, with error -EAGAIN. */
	struct {
		uint32_t request, completed, generation;
		int32_t error;
	} reset;
};
