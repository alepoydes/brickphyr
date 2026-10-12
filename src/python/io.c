// SPDX-License-Identifier: Apache-2.0
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "application_private.h"
#include <brickphyr.h>
#include <zephyr/sys/util.h>
#include <limits.h>

int mp_hal_stdin_rx_chr(void)
{
	for (;;) {
		mp_event_handle_nowait();
		uint8_t value;
		if (application_stdin(&value, 1) == 1) {
			return value;
		}
		python_event_wait(-1);
	}
}

uintptr_t mp_hal_stdio_poll(uintptr_t flags)
{
	uintptr_t ready = (flags & MP_STREAM_POLL_WR) && application_stdout(NULL, 0) ?
		MP_STREAM_POLL_WR : 0;
	if ((flags & MP_STREAM_POLL_RD) && application_stdin(NULL, 0)) {
		ready |= MP_STREAM_POLL_RD;
	}
	return ready;
}

void python_event_wait(int64_t timeout_ms)
{
	(void)application_event_wait(timeout_ms < 0 ? -1 : MIN(timeout_ms, INT32_MAX));
}

mp_uint_t mp_hal_ticks_ms(void)
{
	return bp_time_ms();
}

mp_uint_t mp_hal_stdout_tx_strn(const char *text, size_t length)
{
	size_t written = 0;
	while (length) {
		mp_event_handle_nowait();
		size_t count = MIN(length, 128);
		int accepted = application_stdout(text, count);
		if (accepted < 0) {
			mp_raise_OSError(-accepted);
		}
		written += accepted;
		text += accepted;
		length -= accepted;
		if (!accepted) {
			python_event_wait(-1);
		}
	}
	return written;
}
