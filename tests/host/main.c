// SPDX-License-Identifier: Apache-2.0
#include "host_transport.h"
#include "host_protocol.h"
#include <pbio/protocol.h>

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/ztest.h>

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(test_uart));
static unsigned int calls;

static int cmd_mark(const struct shell *shell, size_t argc, char **argv)
{
	calls++;
	shell_print(shell, "MARK %u", calls);
	return 0;
}
SHELL_CMD_ARG_REGISTER(mark, NULL, "Test observable command execution.", cmd_mark, 1, 0);

static void wait_idle(void)
{
	uint8_t state[8];
	int64_t deadline = k_uptime_get() + 1000;
	do {
		host_interface_status(state);
		if (!state[2] && state[3]) {
			return;
		}
		k_msleep(1);
	} while (k_uptime_get() < deadline);
	zassert_unreachable("Session failed to close");
}

static void send(const void *data, size_t length)
{
	zassert_equal(uart_emul_put_rx_data(uart, data, length), length);
}

static void receive_until(const char *ending, char *output, size_t capacity)
{
	int64_t deadline = k_uptime_get() + 1000;
	size_t length = 0;
	output[0] = 0;
	while (k_uptime_get() < deadline && length < capacity - 1) {
		length += uart_emul_get_tx_data(uart, output + length, capacity - 1 - length);
		output[length] = 0;
		if (strstr(output, ending)) {
			return;
		}
		k_msleep(1);
	}
	zassert_unreachable("Missing %s in %s", ending, output);
}

static void close_session(void)
{
	host_connection_changed(false);
	/* The host must drain the previous stream before selecting a new interface. */
	uart_emul_flush_tx_data(uart);
	wait_idle();
}

static void *setup(void)
{
	zassert_ok(host_transport_init(uart, 1));
	wait_idle();
	return NULL;
}

ZTEST(host, test_interface_roundtrip)
{
	char output[512];
	uint8_t state[8];
	int64_t before = k_uptime_get();
	host_interface_status(state);
	zassert_ok(host_interface_select(HOST_PYBRICKS));
	zassert_equal(host_interface_select(2), -ENOTSUP);
	zassert_ok(host_interface_select(HOST_SHELL));
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	zassert_equal(host_interface_select(HOST_PYBRICKS), -EBUSY);
	/* Backspace and history pass through the same shell as the physical hub. */
	send("marX\x08k\r", 7);
	receive_until("MARK 1", output, sizeof(output));
	send("\x1b[A\r", 4);
	receive_until("MARK 2", output, sizeof(output));
	/* This unfinished line must never execute in the following shell session. */
	send("mark", 4);
	k_msleep(5);
	close_session();
	zassert_ok(host_interface_select(HOST_PYBRICKS));
	host_connection_changed(true);
	/* Upstream tools/flash/serdev.cobs.pack([READ, GATT, 0x00, 0x2a]). */
	static const uint8_t read_name[] = {0x5b, 0x00, 0x00, 0x07, 0x29, 0x02};
	send(read_name, sizeof(read_name));
	uint8_t reply[64];
	size_t length = 0;
	int64_t deadline = k_uptime_get() + 1000;
	while ((!length || reply[length - 1] != 2) && k_uptime_get() < deadline) {
		length += uart_emul_get_tx_data(uart, reply + length, sizeof(reply) - length);
		k_msleep(1);
	}
	static const uint8_t name_reply[] = {
		0x5b, 0x00, 0x00, 0x0e, 0x29, 0x41, 0x71, 0x6a,
		0x60, 0x68, 0x73, 0x6b, 0x7a, 0x71, 0x02,
	};
	zassert_equal(length, sizeof(name_reply));
	zassert_mem_equal(reply, name_reply, length);
	zassert_equal(host_interface_select(HOST_SHELL), -EBUSY);
	close_session();
	zassert_ok(host_interface_select(HOST_SHELL));
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	send("\r", 1);
	k_msleep(5);
	zassert_equal(calls, 2, "Partial command survived handover");
	send("mark\r", 5);
	receive_until("MARK 3", output, sizeof(output));
	zassert_true(k_uptime_get() > before);
	/* Bytes still in the UART must keep selection blocked after DTR drops. */
	send("mark\r", 5);
	k_msleep(5);
	host_connection_changed(false);
	k_msleep(5);
	host_interface_status(state);
	zassert_false(state[3]);
	zassert_equal(host_interface_select(HOST_PYBRICKS), -EBUSY);
	uart_emul_flush_tx_data(uart);
	wait_idle();

	/* Rapid reopen must discard the previous shell parser and pending line. */
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	send("ma", 2);
	k_msleep(5);
	host_connection_changed(false);
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	send("rk\r", 3);
	receive_until("not found", output, sizeof(output));
	zassert_equal(calls, 4);
	close_session();
}

ZTEST(host, test_owner_command_handover)
{
	char output[512];
	uint8_t state[8];
	zassert_ok(host_interface_select(HOST_SHELL));
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	send("host_mode invalid\r", strlen("host_mode invalid\r"));
	receive_until("Usage:", output, sizeof(output));
	close_session();
	host_interface_status(state);
	zassert_equal(state[1], HOST_SHELL);
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	send("host_mode pybricks\r", strlen("host_mode pybricks\r"));
	receive_until("Close this port.", output, sizeof(output));
	host_interface_status(state);
	zassert_equal(state[1], HOST_SHELL, "Never change a live stream's parser");
	zassert_equal(host_interface_select(HOST_PYBRICKS), -EBUSY);
	close_session();
	host_interface_status(state);
	zassert_equal(state[1], HOST_PYBRICKS);
	host_connection_changed(true);
	uint8_t frame[HOST_PROTOCOL_FRAME_MAX];
	const uint8_t command[] = {17, HOST_COMMAND_EXIT_COMPATIBILITY};
	size_t length = pbio_cobs_encode_prefixed(PBIO_PYBRICKS_OUT_EP_MSG_COMMAND,
		command, sizeof(command), frame);
	send(frame, length);
	uint8_t reply[HOST_PROTOCOL_FRAME_MAX];
	length = 0;
	int64_t deadline = k_uptime_get() + 1000;
	while ((!length || reply[length - 1] != PBIO_COBS_DELIMITER) &&
	       k_uptime_get() < deadline) {
		length += uart_emul_get_tx_data(uart, reply + length, sizeof(reply) - length);
		k_msleep(1);
	}
	zassert_true(length > 1);
	uint8_t type;
	uint8_t decoded[16];
	zassert_equal(pbio_cobs_decode_prefixed(reply, length - 1, &type,
		decoded, sizeof(decoded)), 5);
	const uint8_t expected[] = {17, 0, 0, 0, 0};
	zassert_equal(type, PBIO_PYBRICKS_IN_EP_MSG_RESPONSE);
	zassert_mem_equal(decoded, expected, sizeof(expected));
	host_interface_status(state);
	zassert_equal(state[1], HOST_PYBRICKS);
	/* Trailing partial input belongs to the old stream and must be discarded. */
	send(frame, 2);
	close_session();
	host_interface_status(state);
	zassert_equal(state[1], HOST_SHELL);
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	send("host_mode invalid\r", strlen("host_mode invalid\r"));
	receive_until("Usage:", output, sizeof(output));
	close_session();
}

ZTEST_SUITE(host, NULL, setup, NULL, NULL, NULL);
