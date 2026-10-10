// SPDX-License-Identifier: Apache-2.0
#include "host_protocol.h"

#include <string.h>
#include <pbio/protocol.h>
#include <pbio/util.h>
#include <zephyr/app_version.h>

size_t host_protocol_read(uint8_t service, uint16_t id, uint16_t variant,
			  uint16_t max_write, uint8_t value[HOST_PROTOCOL_PAYLOAD_MAX - 3])
{
	const char *text = NULL;

	if (service == PBIO_PYBRICKS_READ_SERVICE_GATT) {
		switch (id) {
		case PBIO_GATT_DEVICE_NAME_CHAR_UUID:
			text = "Brickphyr";
			break;
		case PBIO_GATT_FIRMWARE_VERSION_CHAR_UUID:
			text = APP_VERSION_STRING;
			break;
		case PBIO_GATT_SOFTWARE_VERSION_CHAR_UUID:
			text = PBIO_PROTOCOL_VERSION_STR;
			break;
		case PBIO_GATT_PNP_ID_CHAR_UUID:
			pbio_pybricks_pnp_id(value, PBIO_PYBRICKS_HUB_KIND_PRIME, variant);
			return PBIO_PYBRICKS_PNP_ID_SIZE;
		}
	} else if (service == PBIO_PYBRICKS_READ_SERVICE_PYBRICKS && id == 0x0003) {
		/* No engines, user-program storage or program slots yet. */
		pbio_pybricks_hub_capabilities(value, max_write, 0, 0, 0);
		return PBIO_PYBRICKS_HUB_CAPABILITIES_VALUE_SIZE;
	}
	if (!text) {
		return 0;
	}
	size_t length = strlen(text);
	if (length > HOST_PROTOCOL_PAYLOAD_MAX - 3) {
		length = HOST_PROTOCOL_PAYLOAD_MAX - 3;
	}
	memcpy(value, text, length);
	if (service == PBIO_PYBRICKS_READ_SERVICE_GATT &&
	    id == PBIO_GATT_FIRMWARE_VERSION_CHAR_UUID) {
		/* PEP 440 clients accept our bring-up label as local version metadata. */
		uint8_t *separator = memchr(value, '-', length);
		if (separator) {
			*separator = '+';
		}
	}
	return length;
}

uint8_t host_protocol_command(const uint8_t *command, size_t length)
{
	/* No application supervisor yet: even STOP must not imply one. */
	if (!length || command[0] != HOST_COMMAND_EXIT_COMPATIBILITY) {
		return PBIO_PYBRICKS_ERROR_INVALID_COMMAND;
	}
	return length == 1 ? PBIO_PYBRICKS_ERROR_OK : PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED;
}

size_t host_protocol_receive(struct host_protocol *protocol, uint8_t byte,
			    uint16_t hub_variant, uint8_t output[HOST_PROTOCOL_FRAME_MAX])
{
	if (byte != PBIO_COBS_DELIMITER) {
		if (protocol->length < sizeof(protocol->frame)) {
			protocol->frame[protocol->length++] = byte;
		} else {
			protocol->overflow = true;
		}
		return 0;
	}
	uint8_t type;
	uint8_t message[HOST_PROTOCOL_PAYLOAD_MAX];
	size_t length = protocol->overflow ? 0 : pbio_cobs_decode_prefixed(
		protocol->frame, protocol->length, &type, message, sizeof(message));
	bool empty = protocol->length == 0;
	protocol->length = 0;
	protocol->overflow = false;
	if (!length) {
		protocol->rejected_frames += !empty;
		return 0;
	}

	uint8_t reply[HOST_PROTOCOL_PAYLOAD_MAX];
	if (type == PBIO_PYBRICKS_OUT_EP_MSG_READ && length == 3) {
		memcpy(reply, message, 3);
		length = 3 + host_protocol_read(message[0], pbio_get_uint16_le(message + 1),
			hub_variant, HOST_PROTOCOL_PAYLOAD_MAX - 1, reply + 3);
		return pbio_cobs_encode_prefixed(PBIO_PYBRICKS_IN_EP_MSG_READ_REPLY,
						reply, length, output);
	}
	if (type == PBIO_PYBRICKS_OUT_EP_MSG_COMMAND && length >= 2) {
		reply[0] = message[0];
		uint8_t error = host_protocol_command(message + 1, length - 1);
		if (!error && message[1] == HOST_COMMAND_EXIT_COMPATIBILITY) {
			protocol->exit_compatibility = true;
		}
		pbio_set_uint32_le(reply + 1, error);
		return pbio_cobs_encode_prefixed(PBIO_PYBRICKS_IN_EP_MSG_RESPONSE,
						reply, 5, output);
	}
	if (type == PBIO_PYBRICKS_OUT_EP_MSG_SUBSCRIBE && length == 1 && message[0] <= 1) {
		if (!message[0]) {
			return 0;
		}
		/* No running program; the only implemented status is USB ownership. */
		length = pbio_pybricks_event_status_report(reply,
			1U << PBIO_PYBRICKS_STATUS_USB_HOST_CONNECTED, 0, 0);
		return pbio_cobs_encode_prefixed(PBIO_PYBRICKS_IN_EP_MSG_EVENT,
						reply, length, output);
	}
	/* The upstream framing has no error reply for malformed reads or envelopes. */
	protocol->rejected_frames++;
	return 0;
}
