// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pbio/cobs.h>

/* No application transfer is accepted; this bounds identification and errors. */
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
};

/* Consume one byte. A nonzero result is one complete framed reply in output.
 * Reset the structure between sessions. The caller owns all storage. */
size_t host_protocol_receive(struct host_protocol *protocol, uint8_t byte,
			    uint16_t hub_variant, uint8_t output[HOST_PROTOCOL_FRAME_MAX]);
