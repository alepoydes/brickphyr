// SPDX-License-Identifier: Apache-2.0
#include "host_transport.h"
#include "host_protocol.h"
#include "hub_power.h"
#include <pbio/protocol.h>

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/serial.h>
#include <zephyr/shell/shell.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
#include <zcbor_decode.h>

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

static struct net_buf *receive_smp(void)
{
	struct mcumgr_serial_rx_ctxt context = {0};
	uint8_t line[MCUMGR_SERIAL_MAX_FRAME + 64];
	size_t length = 0;
	struct net_buf *packet = NULL;
	int64_t deadline = k_uptime_get() + 1000;

	while (k_uptime_get() < deadline) {
		uint8_t byte;
		if (!uart_emul_get_tx_data(uart, &byte, 1)) {
			k_msleep(1);
			continue;
		}
		zassert_true(length < sizeof(line));
		line[length++] = byte;
		if (byte == '\n') {
			/* A startup prompt may precede the first SMP response. */
			uint8_t *start = memchr(line, context.nb ? MCUMGR_SERIAL_HDR_FRAG_1 :
					       MCUMGR_SERIAL_HDR_PKT_1, length);
			if (start) {
				packet = mcumgr_serial_process_frag(&context, start,
					length - (start - line));
				if (packet) {
					return packet;
				}
			}
			length = 0;
		}
	}
	zassert_unreachable("No complete SMP response");
	return NULL;
}

ZTEST(host, test_smp_before_shell_prompt)
{
	zassert_ok(host_interface_select(HOST_SHELL));
	k_sched_lock();
	host_connection_changed(true);
	/* Send as soon as DTR rises, without waiting for a human Shell prompt. */
	const char echo[] = "\x06\tABcKAAANAAAAAKFhZGlicmlja3BoeXJYww==\n";
	send(echo, sizeof(echo) - 1);
	k_sched_unlock();
	struct net_buf *packet = receive_smp();
	zassert_equal(packet->data[0], 0x0b);
	smp_packet_free(packet);
	close_session();
}

K_SEM_DEFINE(management_entered, 0, 1);
K_SEM_DEFINE(management_release, 0, 1);
static unsigned int management_calls;

static int blocking_management(struct smp_streamer *stream)
{
	management_calls++;
	k_sem_give(&management_entered);
	zassert_ok(k_sem_take(&management_release, K_SECONDS(1)));
	return 0;
}

static int send_smp(const void *data, int length)
{
	send(data, length);
	return 0;
}

static int ble_packet_begin(const struct net_buf *packet, void **context)
{
	return host_ble_request_begin() ? 0 : MGMT_ERR_EACCESSDENIED;
}

static void ble_packet_end(void *context, int result)
{
	host_ble_request_end();
}

static bool close_on_reply;

static int discard_ble_response(struct net_buf *packet)
{
	zassert_true(host_ble_request_active());
	smp_packet_free(packet);
	if (close_on_reply) {
		host_ble_release(true);
		return 0;
	}
	return MGMT_ERR_EUNKNOWN; /* Synthetic peer disconnected during the handler. */
}

static int ble_packet_result;
static void process_ble_packet(void *a, void *b, void *c)
{
	struct smp_transport transport = {
		.functions = { .output = discard_ble_response,
			.request_begin = ble_packet_begin, .request_end = ble_packet_end },
	};
	struct cbor_nb_reader reader;
	struct cbor_nb_writer writer;
	struct smp_streamer streamer = {
		.smpt = &transport, .reader = &reader, .writer = &writer,
	};
	const uint8_t request[] = { 0x0a, 0, 0, 1, 0, MGMT_GROUP_ID_PERUSER, 3, 0, 0xa0 };
	struct net_buf *packet = smp_packet_alloc();

	zassert_not_null(packet);
	net_buf_add_mem(packet, request, sizeof(request));
	if (close_on_reply) {
		net_buf_add_mem(packet, request, sizeof(request));
	}
	ble_packet_result = smp_process_request_packet(&streamer, packet);
}

ZTEST(host, test_smp_ble_inflight_disconnect)
{
	static const struct mgmt_handler handlers[] = { { .mh_write = blocking_management } };
	static struct mgmt_group group = {
		.mg_handlers = handlers, .mg_handlers_count = ARRAY_SIZE(handlers),
		.mg_group_id = MGMT_GROUP_ID_PERUSER,
	};
	static K_THREAD_STACK_DEFINE(stack, 4096);
	static struct k_thread worker;
	uint8_t state[8];
	uint8_t seconds[4];

	management_calls = 0;
	mgmt_register_group(&group);
	wait_idle();
	zassert_ok(host_ble_acquire());
	sys_put_le32(1, seconds);
	zassert_ok(settings_runtime_set("power/usb_idle_s", seconds, sizeof(seconds)));
	int64_t now = k_uptime_get();
	zassert_false(hub_power_update(1, false, now));
	k_thread_create(&worker, stack, K_THREAD_STACK_SIZEOF(stack), process_ble_packet,
		NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&management_entered, K_SECONDS(1)));
	zassert_false(host_ble_request_active(), "Grant leaked to a different thread");
	host_ble_release(true);
	host_interface_status(state);
	zassert_true(state[2], "Released owner while accepted request still runs");
	zassert_equal(host_ble_acquire(), -EBUSY);
	zassert_equal(host_interface_select(HOST_PYBRICKS), -EBUSY);
	zassert_false(host_ble_request_begin(), "Admitted packet after disconnect");
	zassert_false(hub_power_update(1, false, now + 2000));
	sys_put_le32(0, seconds);
	zassert_ok(settings_runtime_set("power/usb_idle_s", seconds, sizeof(seconds)));
	k_sem_give(&management_release);
	zassert_ok(k_thread_join(&worker, K_SECONDS(1)));
	zassert_equal(ble_packet_result, MGMT_ERR_EUNKNOWN);
	zassert_equal(management_calls, 1);
	wait_idle();
	zassert_equal(host_interface_current(), HOST_SHELL);
	/* A stale queued packet reaches admission after closing; no handler runs. */
	process_ble_packet(NULL, NULL, NULL);
	zassert_equal(ble_packet_result, MGMT_ERR_EACCESSDENIED);
	zassert_equal(management_calls, 1);
	mgmt_unregister_group(&group);
}

static int count_management(struct smp_streamer *stream)
{
	management_calls++;
	return 0;
}

ZTEST(host, test_smp_ble_no_new_command_after_disconnect)
{
	static const struct mgmt_handler handlers[] = { { .mh_write = count_management } };
	static struct mgmt_group group = {
		.mg_handlers = handlers, .mg_handlers_count = ARRAY_SIZE(handlers),
		.mg_group_id = MGMT_GROUP_ID_PERUSER,
	};
	management_calls = 0;
	mgmt_register_group(&group);
	zassert_ok(host_ble_acquire());
	/* Disconnect just after one response: the next command in the same packet
	 * must not run under the previous command's lifetime grant. */
	close_on_reply = true;
	process_ble_packet(NULL, NULL, NULL);
	close_on_reply = false;
	zassert_equal(ble_packet_result, MGMT_ERR_EACCESSDENIED);
	zassert_equal(management_calls, 1);
	mgmt_unregister_group(&group);
	wait_idle();
}

ZTEST(host, test_smp_inflight_disconnect)
{
	management_calls = 0;
	/* A synthetic slow operation exercises the real SMP processor and teardown. */
	static const struct mgmt_handler handlers[] = { { .mh_write = blocking_management } };
	static struct mgmt_group group = {
		.mg_handlers = handlers, .mg_handlers_count = ARRAY_SIZE(handlers),
		.mg_group_id = MGMT_GROUP_ID_PERUSER,
	};
	char output[512];

	mgmt_register_group(&group);
	zassert_ok(host_interface_select(HOST_SHELL));
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	/* Two standard SMP v2 WRITE requests, each carrying an empty CBOR map. */
	const uint8_t requests[] = {
		0x0a, 0, 0, 1, 0, MGMT_GROUP_ID_PERUSER, 1, 0, 0xa0,
		0x0a, 0, 0, 1, 0, MGMT_GROUP_ID_PERUSER, 2, 0, 0xa0,
	};
	zassert_ok(mcumgr_serial_tx_pkt(requests, sizeof(requests), send_smp));
	zassert_ok(k_sem_take(&management_entered, K_SECONDS(1)));
	host_connection_changed(false);
	zassert_equal(host_interface_select(HOST_PYBRICKS), -EBUSY);
	host_connection_changed(true);
	k_sem_give(&management_release);
	receive_until("brickphyr> ", output, sizeof(output));
	zassert_equal(management_calls, 1, "Old queued request executed after disconnect");
	zassert_is_null(memchr(output, MCUMGR_SERIAL_HDR_PKT_1, strlen(output)),
			"Old SMP response leaked into the new session");
	/* The next session must admit a fresh operation. */
	k_sem_give(&management_release);
	zassert_ok(mcumgr_serial_tx_pkt(requests, 9, send_smp));
	struct net_buf *packet = receive_smp();

	zassert_equal(packet->data[6], 1);
	zassert_equal(management_calls, 2);
	smp_packet_free(packet);
	close_session();
	mgmt_unregister_group(&group);
}

ZTEST(host, test_smp_settings_and_session_reset)
{
	char output[512];
	zassert_ok(host_interface_select(HOST_SHELL));
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	/* Wire vectors from smpclient 7.3.0 requests + smp.packet.encode(). */
	const char echo[] = "\x06\tABcKAAANAAAAAKFhZGlicmlja3BoeXJYww==\n";
	const char write[] = "\x06\tACoKAAAgAAMBAKJjdmFsRBEAAABkbmFtZXBwb3dlci91c2JfaWRsZV9zKbs=\n";
	const char read[] = "\x06\tACEIAAAXAAMCAKFkbmFtZXBwb3dlci91c2JfaWRsZV9zM6E=\n";
	const char save[] = "\x06\tAAsKAAABAAMDA6CIDQ==\n";
	send(echo, sizeof(echo) - 1);
	struct net_buf *packet = receive_smp();
	zcbor_state_t state[4];
	struct zcbor_string text;
	zcbor_new_decode_state(state, ARRAY_SIZE(state), packet->data + 8, packet->len - 8, 1, NULL, 0);
	zassert_true(zcbor_map_start_decode(state));
	zassert_true(zcbor_tstr_expect_lit(state, "r"));
	zassert_true(zcbor_tstr_decode(state, &text));
	zassert_equal(text.len, strlen("brickphyr"));
	zassert_mem_equal(text.value, "brickphyr", text.len);
	smp_packet_free(packet);
	send(write, sizeof(write) - 1);
	packet = receive_smp();
	zassert_equal(packet->data[6], 1, "Wrong reply sequence");
	smp_packet_free(packet);
	uint8_t value[4];
	zassert_equal(settings_runtime_get("power/usb_idle_s", value, sizeof(value)), 4);
	zassert_equal(sys_get_le32(value), 17);
	send(read, sizeof(read) - 1);
	packet = receive_smp();
	zassert_equal(packet->data[6], 2);
	zcbor_new_decode_state(state, ARRAY_SIZE(state), packet->data + 8, packet->len - 8, 1, NULL, 0);
	zassert_true(zcbor_map_start_decode(state));
	zassert_true(zcbor_tstr_expect_lit(state, "val"));
	zassert_true(zcbor_bstr_decode(state, &text));
	zassert_equal(text.len, 4);
	zassert_equal(sys_get_le32(text.value), 17);
	smp_packet_free(packet);
	/* No storage in this configuration: save must report an error. */
	send(save, sizeof(save) - 1);
	packet = receive_smp();
	zcbor_new_decode_state(state, ARRAY_SIZE(state), packet->data + 8, packet->len - 8, 1, NULL, 0);
	zassert_true(zcbor_map_start_decode(state));
	zassert_true(zcbor_tstr_expect_lit(state, "rc"));
	uint32_t result;
	zassert_true(zcbor_uint32_decode(state, &result));
	zassert_not_equal(result, 0);
	smp_packet_free(packet);
	send("config get power/usb_idle_s\r", strlen("config get power/usb_idle_s\r"));
	receive_until("brickphyr> ", output, sizeof(output));
	zassert_not_null(strstr(output, "17 s (selected and effective)"));
	send("config set power/usb_idle_s 0\r", strlen("config set power/usb_idle_s 0\r"));
	receive_until("brickphyr> ", output, sizeof(output));
	send(write, 9);
	k_msleep(5);
	close_session();
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	send(write + 9, sizeof(write) - 1 - 9);
	receive_until("brickphyr> ", output, sizeof(output));
	zassert_equal(settings_runtime_get("power/usb_idle_s", value, sizeof(value)), 4);
	zassert_equal(sys_get_le32(value), 0, "Partial SMP command crossed session boundary");
	send(echo, sizeof(echo) - 1);
	packet = receive_smp();
	zassert_equal(packet->data[6], 0);
	smp_packet_free(packet);
	close_session();
}

ZTEST(host, test_ble_usb_exclusion)
{
	char output[512];
	uint8_t state[8];
	uint8_t seconds[4];
	wait_idle();
	zassert_ok(host_interface_select(HOST_SHELL));
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	zassert_equal(host_ble_acquire(), -EBUSY);
	close_session();
	zassert_ok(host_ble_acquire());
	zassert_equal(host_ble_acquire(), -EBUSY);
	zassert_equal(host_interface_select(HOST_PYBRICKS), -EBUSY);
	host_interface_status(state);
	zassert_true(state[2]);
	zassert_false(state[3]);
	sys_put_le32(1, seconds);
	zassert_ok(settings_runtime_set("power/usb_idle_s", seconds, sizeof(seconds)));
	int64_t now = k_uptime_get();
	zassert_false(hub_power_update(1, false, now));
	/* A rejected USB open/close must not release the BLE power hold. */
	host_connection_changed(true);
	host_connection_changed(false);
	zassert_false(hub_power_update(1, false, now + 2000));
	host_connection_changed(true);
	unsigned int before = calls;
	send("mark\r", 5);
	k_msleep(5);
	host_ble_release(true);
	zassert_equal(host_interface_current(), HOST_SHELL);
	host_connection_changed(true); /* repeated DTR-high is not a fresh open */
	zassert_equal(host_interface_select(HOST_PYBRICKS), -EBUSY);
	zassert_equal(host_ble_acquire(), -EBUSY);
	sys_put_le32(0, seconds);
	zassert_ok(settings_runtime_set("power/usb_idle_s", seconds, sizeof(seconds)));
	close_session();
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	send("\r", 1);
	receive_until("brickphyr> ", output, sizeof(output));
	zassert_equal(calls, before, "Rejected USB input reached a later session");
	close_session();
}

ZTEST(host, test_zpower_settings_and_idle_claim)
{
	char output[512];
	zassert_ok(host_interface_select(HOST_SHELL));
	host_connection_changed(true);
	receive_until("brickphyr> ", output, sizeof(output));
	const char *set = "config set power/usb_idle_s 2\r";
	send(set, strlen(set));
	receive_until("Applied in RAM", output, sizeof(output));
	const char *invalid = "config set power/usb_idle_s -1\r";
	send(invalid, strlen(invalid));
	receive_until("Use integer seconds", output, sizeof(output));
	send("config get power\r", strlen("config get power\r"));
	receive_until("Setting operation failed", output, sizeof(output));
	send("config save\r", strlen("config save\r"));
	receive_until("not available", output, sizeof(output));
	uint8_t value[4];
	zassert_equal(settings_runtime_get("power/usb_idle_s", value, sizeof(value)), 4);
	zassert_equal(sys_get_le32(value), 2, "Rejected write changed the setting");
	zassert_equal(settings_runtime_set("power/usb_idle_s", value, 3), -EINVAL);
	zassert_equal(settings_runtime_get("power/unknown", value, sizeof(value)), -ENOENT);
	zassert_equal(settings_runtime_get("power/usb_idle_s", value, 3), -ENOSPC);
	/* Synthetic time exercises the policy; host acquisition is the real DTR path. */
	int64_t base = k_uptime_get();
	zassert_false(hub_power_update(1, false, base));
	zassert_false(hub_power_update(1, false, base + 10000), "Owner must inhibit expiry");
	close_session();
	base = k_uptime_get();
	zassert_false(hub_power_update(1, false, base + 1000));
	zassert_false(hub_power_update(1, true, base + 1500));
	zassert_false(hub_power_update(1, false, base + 3000), "Activity restarts idle time");
	zassert_false(hub_power_update(0, false, base + 10000), "Battery timeout is independent");
	zassert_false(hub_power_update(0, false, base + 100000), "Zero disables expiry");
	sys_put_le32(UINT32_MAX, value);
	zassert_ok(settings_runtime_set("power/battery_idle_s", value, sizeof(value)));
	zassert_false(hub_power_update(0, false, base + 100000), "Seconds conversion overflowed");
	zassert_false(hub_power_update(-EIO, false, base + 1000000));
	zassert_false(hub_power_update(1, false, base + 1001000));
	zassert_false(hub_power_update(1, false, base + 1002999));
	zassert_true(hub_power_update(1, false, base + 1003000));
	zassert_false(hub_power_connection(true), "A claimed shutdown must reject a late owner");
}

ZTEST_SUITE(host, NULL, setup, NULL, NULL, NULL);
