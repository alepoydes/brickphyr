// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pbio/cobs.h>

/* Bounded command/event fragments; application transfers span many frames. */
#define HOST_PROTOCOL_PAYLOAD_MAX 64
#define HOST_PROTOCOL_FRAME_MAX PBIO_COBS_ENCODED_BUFFER_SIZE(HOST_PROTOCOL_PAYLOAD_MAX)

/* Brickphyr extension, not an upstream Pybricks command. No payload. */
#define HOST_COMMAND_EXIT_COMPATIBILITY 0x80

struct host_protocol {
	uint8_t frame[HOST_PROTOCOL_FRAME_MAX];
	size_t length;
	bool overflow;
	bool exit_compatibility;
	uint32_t rejected_frames;
	bool subscribed;
	bool status_dirty;
	bool start_pending;
	bool downloading;
	size_t download_bytes;
	uint32_t last_status;
	uint8_t program_id;
	uint8_t download_slot;
	uint8_t last_slot;
};

void host_protocol_reset(struct host_protocol *protocol);
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
uint8_t host_protocol_selected_slot(void);
int host_protocol_select_slot(unsigned int slot);
#endif

/* Consume one byte. A nonzero result is one complete framed reply in output.
 * Reset the structure between sessions. The caller owns all storage. */
size_t host_protocol_receive(struct host_protocol *protocol, uint8_t byte,
			    uint16_t hub_variant, uint8_t output[HOST_PROTOCOL_FRAME_MAX]);

/* One transport serializes commands and event collection for its session.
 * Event bytes remain owned by that transport until sent or disconnected. */
size_t host_protocol_read(uint8_t service, uint16_t id, uint16_t variant,
			  uint16_t max_write, uint8_t value[HOST_PROTOCOL_PAYLOAD_MAX - 3]);
uint8_t host_protocol_command(struct host_protocol *protocol, const uint8_t *command, size_t length);
size_t host_protocol_event(struct host_protocol *protocol, bool ble,
	uint8_t *output, size_t capacity);
