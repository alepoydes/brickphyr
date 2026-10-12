// SPDX-License-Identifier: Apache-2.0
#include "host_protocol.h"
#include <assert.h>
#include <pbio/protocol.h>
#include <stdio.h>

static void status_timing(void)
{
	struct host_protocol protocol = {.subscribed = true, .status_dirty = true};
	uint8_t event[HOST_PROTOCOL_PAYLOAD_MAX];
	assert(host_protocol_event(&protocol, true, 1000, event, sizeof(event)) ==
		PBIO_PYBRICKS_EVENT_STATUS_REPORT_SIZE);
	assert(event[0] == PBIO_PYBRICKS_EVENT_STATUS_REPORT);
	assert(!host_protocol_event(&protocol, true, 1499, event, sizeof(event)));
	assert(host_protocol_event(&protocol, true, 1500, event, sizeof(event)) ==
		PBIO_PYBRICKS_EVENT_STATUS_REPORT_SIZE);
	/* State changes are immediate and restart the repeat interval. */
	protocol.status_dirty = true;
	assert(host_protocol_event(&protocol, false, UINT32_MAX - 99, event, sizeof(event)));
	assert(!host_protocol_event(&protocol, false, 399, event, sizeof(event)));
	assert(host_protocol_event(&protocol, false, 400, event, sizeof(event)));
	protocol.subscribed = false;
	assert(!host_protocol_event(&protocol, false, 1000, event, sizeof(event)));
}

int main(int argc, char **argv)
{
	(void)argv;
	if (argc > 1) {
		status_timing();
		return 0;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	struct host_protocol protocol = {0};
	uint8_t reply[HOST_PROTOCOL_FRAME_MAX];
	int byte;

	while ((byte = getchar()) != EOF) {
		size_t length = host_protocol_receive(&protocol, byte, 1, 0, reply);
		if (length) {
			fwrite(reply, 1, length, stdout);
		}
	}
	return ferror(stdin) || ferror(stdout);
}
