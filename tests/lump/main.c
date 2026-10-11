// SPDX-License-Identifier: Apache-2.0
#include "lump.h"
#include "captures.h"
#include "hub_ports.h"
#include "wiring.h"
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

static const uint8_t speed[] = {0x52, 0, 0xc2, 1, 0, 0x6e};
static const uint8_t position[] = {0xd2, 0x34, 0x12, 0, 0, 0x0b};

static int parse_bytes(struct lump *l, const uint8_t *bytes, size_t count)
{
	int result = 0;
	for (size_t i = 0; i < count; i++) {
		result = lump_receive(l, bytes[i]);
		if (result < 0) {
			return result;
		}
	}
	return result;
}

ZTEST(lump, test_captured_metadata)
{
	const struct { const uint8_t *bytes; size_t count; uint8_t id, modes; } cases[] = {
		{boost_color_distance_sensor, sizeof(boost_color_distance_sensor), 37, 11},
		{boost_interactive_motor, sizeof(boost_interactive_motor), 38, 4},
		{technic_large_motor, sizeof(technic_large_motor), 46, 6},
	};
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		struct lump l;
		lump_reset(&l);
		zassert_ok(parse_bytes(&l, cases[i].bytes, cases[i].count), "capture %zu", i);
		zassert_equal(l.phase, LUMP_ACK);
		zassert_equal(l.id, cases[i].id);
		zassert_equal(l.num_modes, cases[i].modes);
		zassert_equal(l.baud, 115200);
		zassert_equal(l.sequence, 0);
	}
	struct lump l;
	lump_reset(&l);
	zassert_ok(parse_bytes(&l, boost_interactive_motor, sizeof(boost_interactive_motor)));
	zassert_mem_equal(l.modes[2].name, "POS", 4);
	zassert_equal(l.modes[2].type, LUMP_DATA_TYPE_DATA32);
	zassert_equal(l.modes[2].values, 1);
	l.phase = LUMP_DATA;
	zassert_ok(parse_bytes(&l, position, sizeof(position) - 1));
	zassert_equal(l.sequence, 0, "Partial frame published");
	zassert_equal(lump_receive(&l, position[sizeof(position) - 1]), 1);
	zassert_equal(l.sequence, 1);
	zassert_equal(l.length, 4);
	zassert_mem_equal(l.data, position + 1, 4);
	uint8_t damaged[sizeof(position)];
	memcpy(damaged, position, sizeof(position));
	damaged[2] ^= 1;
	zassert_ok(parse_bytes(&l, damaged, sizeof(damaged)));
	zassert_equal(l.sequence, 1);
	zassert_equal(l.bad_frames, 1);
	zassert_mem_equal(l.data, position + 1, 4);
	zassert_equal(parse_bytes(&l, position, sizeof(position)), 1);

	lump_reset(&l);
	zassert_ok(parse_bytes(&l, boost_color_distance_sensor, sizeof(boost_color_distance_sensor)));
	l.phase = LUMP_DATA;
	const uint8_t extended[] = {0x46, 8, 0xb1, 0xd0, 0, 0, 0, 0, 0x2f};
	zassert_equal(parse_bytes(&l, extended, sizeof(extended)), 1);
	zassert_equal(l.mode, 8);
	zassert_equal(l.length, 4);
	uint8_t command[34];
	zassert_equal(lump_command(command, LUMP_CMD_SPEED, speed + 1, 4), sizeof(speed));
	zassert_mem_equal(command, speed, sizeof(speed));
	zassert_equal(lump_command(command, LUMP_CMD_SPEED, speed, 33), -EINVAL);
}

ZTEST(lump, test_invalid_metadata)
{
	/* Independently constructed malformed wire packets: a short SPEED,
	 * short FORMAT, too many modes, oversized frame and premature ACK. */
	const uint8_t packets[][7] = {
		{0x42, 0, 0xbd}, {0x80, 0x80, 1, 0xfe},
		{0x51, 7, 7, 16, 7, 0xb9}, {0xb0}, {0x04},
	};
	const size_t sizes[] = {3, 4, 6, 1, 1};
	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		struct lump l;
		lump_reset(&l);
		zassert_ok(parse_bytes(&l, boost_interactive_motor, 3));
		zassert_true(parse_bytes(&l, packets[i], sizes[i]) < 0, "case %zu", i);
		zassert_equal(l.sequence, 0);
	}
	struct lump l;
	lump_reset(&l);
	const uint8_t bad_type[] = {0x40, 38, 0};
	zassert_ok(parse_bytes(&l, bad_type, sizeof(bad_type)));
	zassert_equal(l.phase, LUMP_TYPE);
	zassert_ok(parse_bytes(&l, boost_interactive_motor, sizeof(boost_interactive_motor)));
	zassert_equal(l.phase, LUMP_ACK);

	lump_reset(&l);
	zassert_ok(parse_bytes(&l, boost_interactive_motor, 7));
	const uint8_t name[] = {0x80, 0, 'X', 0x27};
	zassert_ok(parse_bytes(&l, name, sizeof(name)));
	const uint8_t too_many_values[] = {0x90, 0x80, 33, 0, 0, 0, 0xce};
	zassert_equal(parse_bytes(&l, too_many_values, sizeof(too_many_values)), -EBADMSG);
}

static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(lump_uart));
static uint8_t tx[128];
static size_t tx_count;
static void sent(const struct device *dev, size_t size, void *arg)
{
	zassert_true(size <= sizeof(tx) - tx_count);
	tx_count += uart_emul_get_tx_data(dev, tx + tx_count, sizeof(tx) - tx_count);
}

static struct bp_port_state snapshot(void)
{
	struct bp_port_state value;
	zassert_ok(hub_port_get(0, &value));
	return value;
}

static void wait_tx(size_t count)
{
	int64_t deadline = k_uptime_get() + 1000;
	while (tx_count < count) {
		zassert_true(k_uptime_get() < deadline, "Missing TX; phase=%u error=%d", snapshot().phase, snapshot().error);
		k_msleep(1);
	}
}

static void feed(const uint8_t *data, size_t count)
{
	/* Chunking also exercises partial frames. One byte / 5 ms is within the
	 * slow 2400-baud handshake, without reproducing the service's parser. */
	for (size_t i = 0; i < count; i++) {
		zassert_equal(uart_emul_put_rx_data(uart, data + i, 1), 1);
		k_msleep(5);
	}
}

static struct bp_port_state wait_phase(enum bp_port_phase phase)
{
	int64_t deadline = k_uptime_get() + 1000;
	struct bp_port_state v;
	do {
		v = snapshot();
		zassert_true(k_uptime_get() < deadline, "phase=%u error=%d, wanted=%u", v.phase, v.error, phase);
		if (v.phase == phase) {
			return v;
		}
		k_msleep(1);
	} while (true);
}

static void negotiate(bool fast)
{
	tx_count = 0;
	pup_test_circuits[0].kind = UART_DEVICE;
	wait_tx(sizeof(speed));
	zassert_mem_equal(tx, speed, sizeof(speed));
	if (fast) {
		uint8_t ack = LUMP_SYS_ACK;
		feed(&ack, 1);
	} else {
		k_msleep(20);
	}
	struct uart_config cfg;
	zassert_ok(uart_config_get(uart, &cfg));
	zassert_equal(cfg.baudrate, fast ? 115200 : 2400);
	zassert_equal(snapshot().error, -EAGAIN);
	feed(boost_interactive_motor, sizeof(boost_interactive_motor));
	wait_tx(sizeof(speed) + 2);
	zassert_equal(tx[sizeof(speed)], LUMP_SYS_ACK);
	zassert_equal(tx[sizeof(speed) + 1], LUMP_SYS_NACK);
	zassert_ok(uart_config_get(uart, &cfg));
	zassert_equal(cfg.baudrate, 115200);
	zassert_equal(wait_phase(BP_PORT_READY).error, -EAGAIN);
	feed(position, sizeof(position));
	zassert_ok(snapshot().error);
	zassert_equal(snapshot().type_id, 38);
	zassert_equal(snapshot().data_sequence, 1);
	zassert_equal(snapshot().mode, 2);
	zassert_equal(snapshot().length, 4);
	zassert_mem_equal(snapshot().data, position + 1, 4);
}

static void smp_snapshot(void)
{
	/* The registered observation handler must fit the existing transport and
	 * return the exact binary sample seen through the native service. */
	static struct cbor_nb_reader reader;
	static struct cbor_nb_writer writer;
	static uint8_t response[CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE - 8];
	static const uint8_t request[] = {0xa1, 0x66, 's', 'o', 'u', 'r', 'c', 'e',
		0x66, 'p', 'o', 'r', 't', '.', 'A'};
	zcbor_new_decode_state(reader.zs, ARRAY_SIZE(reader.zs), request, sizeof(request), 1, NULL, 0);
	zcbor_new_encode_state(writer.zs, ARRAY_SIZE(writer.zs), response, sizeof(response), 1);
	zassert_true(zcbor_map_start_encode(writer.zs, 5));
	struct smp_streamer stream = {.reader = &reader, .writer = &writer};
	const struct mgmt_handler *handler = mgmt_find_handler(66, 1);
	zassert_not_null(handler);
	zassert_ok(handler->mh_read(&stream));
	zassert_true(zcbor_map_end_encode(writer.zs, 5));
	zcbor_new_decode_state(reader.zs, ARRAY_SIZE(reader.zs), response,
		writer.zs[0].payload - response, 1, NULL, 0);
	zassert_true(zcbor_map_start_decode(reader.zs));
	bool found = false;
	while (!zcbor_array_at_end(reader.zs)) {
		struct zcbor_string key;
		zassert_true(zcbor_tstr_decode(reader.zs, &key));
		if (key.len != 6 || memcmp(key.value, "values", 6)) {
			zassert_true(zcbor_any_skip(reader.zs, NULL));
			continue;
		}
		zassert_true(zcbor_map_start_decode(reader.zs));
		while (!zcbor_array_at_end(reader.zs)) {
			zassert_true(zcbor_tstr_decode(reader.zs, &key));
			if (key.len == 4 && !memcmp(key.value, "data", 4)) {
				struct zcbor_string value;
				zassert_true(zcbor_bstr_decode(reader.zs, &value));
				zassert_equal(value.len, 4);
				zassert_mem_equal(value.value, position + 1, 4);
				found = true;
			} else {
				zassert_true(zcbor_any_skip(reader.zs, NULL));
			}
		}
		zassert_true(zcbor_map_end_decode(reader.zs));
	}
	zassert_true(zcbor_map_end_decode(reader.zs));
	zassert_true(found);
}

ZTEST(lump, test_uart_hotplug)
{
	uart_emul_callback_tx_data_ready_set(uart, sent, NULL);
	/* A peer that sends SYNC forever must eventually release the port.
	 * Accelerated simulator time makes this independent of wall-clock waits. */
	pup_test_circuits[0].kind = UART_DEVICE;
	wait_tx(sizeof(speed));
	uint8_t ack = LUMP_SYS_ACK, sync = LUMP_SYS_SYNC;
	feed(&ack, 1);
	feed(boost_interactive_motor, 3);
	int64_t began = k_uptime_get();
	while (snapshot().phase != BP_PORT_FAULT) {
		zassert_true(k_uptime_get() - began < 40000, "Unbounded metadata negotiation");
		feed(&sync, 1);
		k_msleep(100);
	}
	zassert_true(k_uptime_get() - began > 1000, "Did not exercise active but stalled peer");
	zassert_equal(snapshot().error, -ETIMEDOUT);
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
	/* Likewise, no first TYPE at all must not strand electrical detection. */
	tx_count = 0;
	pup_test_circuits[0].kind = UART_DEVICE;
	wait_tx(sizeof(speed));
	zassert_equal(wait_phase(BP_PORT_FAULT).error, -ETIMEDOUT);
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
	negotiate(false);
	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(shell_execute_cmd(shell, "observe port.A"));
	size_t size;
	const char *output = shell_backend_dummy_get_output(shell, &size);
	zassert_not_null(strstr(output, "type_id=38"));
	zassert_not_null(strstr(output, "data_sequence=1"));
	smp_snapshot();

	struct bp_port_state before = snapshot();
	uint8_t damaged[sizeof(position)];
	memcpy(damaged, position, sizeof(position));
	damaged[1] ^= 1;
	feed(damaged, sizeof(damaged));
	zassert_equal(snapshot().data_sequence, before.data_sequence);
	zassert_equal(snapshot().data_ms, before.data_ms);
	zassert_equal(snapshot().bad_frames, 1);
	uint32_t sequence = snapshot().sequence;
	k_msleep(105);
	zassert_equal(snapshot().sequence, sequence, "Keepalive refreshed sample sequence");
	zassert_true(tx_count >= sizeof(speed) + 3);
	zassert_equal(tx[tx_count - 1], LUMP_SYS_NACK);

	/* UART silence detects unplugging without driving GPIO over an active
	 * serial interface. Previous identity and DATA must disappear together. */
	pup_test_circuits[0].kind = UNPLUGGED;
	struct bp_port_state value = wait_phase(BP_PORT_FAULT);
	zassert_equal(value.error, -ETIMEDOUT);
	zassert_equal(value.length, 0);
	zassert_equal(value.type_id, 0);
	zassert_true(pup_test_released(0));
	wait_phase(BP_PORT_EMPTY);
	negotiate(true);
	zassert_true(snapshot().generation > before.generation);

	uart_emul_set_errors(uart, UART_ERROR_FRAMING);
	feed(position, sizeof(position));
	value = wait_phase(BP_PORT_FAULT);
	zassert_equal(value.error, -EIO);
	zassert_equal(value.length, 0);
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
	negotiate(true);

	/* Burst exceeds the production RX ring, while the emulated UART's FIFO
	 * itself fits it: queue loss must be a fault, not valid new sensor data. */
	uint8_t burst[768] = {0};
	zassert_equal(uart_emul_put_rx_data(uart, burst, sizeof(burst)), sizeof(burst));
	value = wait_phase(BP_PORT_FAULT);
	zassert_equal(value.error, -ENOBUFS);
	zassert_equal(value.length, 0);
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
	negotiate(true);
	zassert_ok(hub_ports_shutdown());
	zassert_equal(snapshot().phase, BP_PORT_OFF);
	zassert_equal(snapshot().length, 0);
	zassert_false(pup_test_supply());
	zassert_true(pup_test_released(0));
	before = snapshot();
	feed(position, sizeof(position));
	zassert_equal(snapshot().sequence, before.sequence);
}

ZTEST_SUITE(lump, NULL, NULL, NULL, NULL, NULL);
