// SPDX-License-Identifier: Apache-2.0
#include "host_protocol.h"
#include <stdio.h>

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	struct host_protocol protocol = {0};
	uint8_t reply[HOST_PROTOCOL_FRAME_MAX];
	int byte;

	while ((byte = getchar()) != EOF) {
		size_t length = host_protocol_receive(&protocol, byte, 1, reply);
		if (length) {
			fwrite(reply, 1, length, stdout);
		}
	}
	return ferror(stdin) || ferror(stdout);
}
