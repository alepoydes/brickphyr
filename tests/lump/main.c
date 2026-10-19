// SPDX-License-Identifier: Apache-2.0
#include "lump.h"
#include "captures.h"
#include "hub_ports.h"
#include "control_frame.h"
#include "port_uart.h"
#include "wiring.h"
#include <lego/device.h>
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>
#include <zephyr/sys/byteorder.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <zephyr/fff.h>

DEFINE_FFF_GLOBALS;

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

static size_t allocated_bytes(void)
{
	struct k_heap *heaps;
	int count = k_heap_array_get(&heaps);
	size_t total = 0;
	for (int i = 0; i < count; i++) {
		struct sys_memory_stats stats;
		k_spinlock_key_t key = k_spin_lock(&heaps[i].lock);
		int err = sys_heap_runtime_stats_get(&heaps[i].heap, &stats);
		k_spin_unlock(&heaps[i].lock, key);
		zassert_ok(err);
		total += stats.allocated_bytes;
	}
	return total;
}

static size_t make_packet(uint8_t frame[35], uint8_t type, uint8_t mode,
	uint8_t info, const uint8_t *data, size_t size)
{
	uint8_t exponent = 0;
	size_t padded = 1;
	while (padded < size) {
		padded *= 2;
		exponent++;
	}
	frame[0] = type | (exponent << 3) | (mode & 7);
	size_t pos = 1;
	if (type == LUMP_MSG_TYPE_INFO) {
		frame[pos++] = info | (mode >= 8 ? LUMP_INFO_MODE_PLUS_8 : 0);
	}
	for (size_t i = 0; i < padded; i++) frame[pos++] = i < size ? data[i] : 0;
	uint8_t checksum = 0xff;
	for (size_t i = 0; i < pos; i++) checksum ^= frame[i];
	frame[pos++] = checksum;
	return pos;
}

static int parse_info(struct lump *l, uint8_t mode, uint8_t info,
	const uint8_t *data, size_t size)
{
	uint8_t frame[35];
	return parse_bytes(l, frame, make_packet(frame, LUMP_MSG_TYPE_INFO, mode, info, data, size));
}

ZTEST(lump, test_captured_metadata)
{
	const struct { const uint8_t *bytes; size_t count; uint8_t id, modes; } cases[] = {
		{boost_color_distance_sensor, sizeof(boost_color_distance_sensor), 37, 11},
		{boost_interactive_motor, sizeof(boost_interactive_motor), 38, 4},
		{technic_large_motor, sizeof(technic_large_motor), 46, 6},
	};
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		static struct lump l;
		lump_reset(&l);
		size_t before = allocated_bytes();
		zassert_ok(parse_bytes(&l, cases[i].bytes, cases[i].count), "capture %zu", i);
		zassert_equal(l.phase, LUMP_ACK);
		zassert_equal(l.id, cases[i].id);
		zassert_equal(l.num_modes, cases[i].modes);
		zassert_equal(l.baud, 115200);
		zassert_equal(l.sequence, 0);
		printk("Captured ID %u, %u modes: %zu allocated bytes\n", l.id, l.num_modes,
			allocated_bytes() - before);
		lump_reset(&l);
		zassert_equal(allocated_bytes(), before);
	}
	static struct lump l;
	lump_reset(&l);
	zassert_ok(parse_bytes(&l, boost_interactive_motor, sizeof(boost_interactive_motor)));
	zassert_mem_equal(l.modes[2].name, "POS", 4);
	zassert_equal(l.modes[2].data_type, LUMP_DATA_TYPE_DATA32);
	zassert_equal(l.modes[2].num_values, 1);
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
	lump_reset(&l);
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
		static struct lump l;
		lump_reset(&l);
		zassert_ok(parse_bytes(&l, boost_interactive_motor, 3));
		zassert_true(parse_bytes(&l, packets[i], sizes[i]) < 0, "case %zu", i);
		zassert_equal(l.sequence, 0);
	}
	static struct lump l;
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
	lump_reset(&l);
}

ZTEST(lump_capacity, test_mode_memory_lifetime)
{
#ifdef UART_CAPACITY_ONLY
	/* Stop the electrical scanner before driving the UART layer directly.
	 * Keep real link allocations live while interleaved parser peers grow
	 * their metadata; this checks heap capacity, not simultaneous wire I/O. */
	zassert_ok(hub_ports_shutdown());
	size_t idle = allocated_bytes();
	for (uint32_t port = 0; port < DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(hub_ports)); port++) {
		zassert_ok(port_uart_start(port));
	}
#endif
	size_t before = allocated_bytes();
	/* Interleave INFO across all ports to exercise simultaneous heap growth.
	 * Each has sixteen modes; the second connection has short and absent INFO
	 * to check expansion into a fresh, zero-tailed public mode. */
	static struct lump peers[DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(hub_ports))];
	uint8_t type[34], modes[34], id = 201;
	const uint8_t counts[] = {7, 0, 15, 0};
	const uint8_t combinations[32] = {
		1, 0, 3, 0, 5, 0, 7, 0, 9, 0, 11, 0, 13, 0, 15, 0,
		17, 0, 19, 0, 21, 0, 23, 0, 25, 0, 27, 0, 29, 0, 0x12, 0x34,
	};
	uint8_t extra[6][32];
	for (size_t i = 0; i < ARRAY_SIZE(extra); i++) {
		for (size_t n = 0; n < sizeof(extra[i]); n++) extra[i][n] = 0x20 * (i + 1) + n;
	}
	const uint8_t format[] = {1, LUMP_DATA_TYPE_DATA8, 3, 0};
	int type_size = lump_command(type, LUMP_CMD_TYPE, &id, 1);
	int modes_size = lump_command(modes, LUMP_CMD_MODES, counts, sizeof(counts));
	for (size_t round = 0; round < 2; round++) {
		for (size_t port = 0; port < ARRAY_SIZE(peers); port++) {
			lump_reset(&peers[port]);
			zassert_ok(parse_bytes(&peers[port], type, type_size));
			zassert_ok(parse_bytes(&peers[port], modes, modes_size));
			zassert_equal(peers[port].num_modes, 16);
		}
		for (uint8_t index = 0; index < 16; index++) {
			uint8_t name[] = {'M', 'A' + index, 0, 0};
			for (size_t port = 0; port < ARRAY_SIZE(peers); port++) {
				zassert_ok(parse_info(&peers[port], index, LUMP_INFO_NAME, name, sizeof(name)));
			}
			for (size_t n = 0; n <= ARRAY_SIZE(extra); n++) {
				for (size_t port = 0; port < ARRAY_SIZE(peers); port++) {
					size_t field = (port + index) & 1 ? ARRAY_SIZE(extra) - n : n;
					if (!field) {
						zassert_ok(parse_info(&peers[port], index, LUMP_INFO_MODE_COMBOS,
							combinations, sizeof(combinations)));
						continue;
					}
					field--;
					if (round && field == 2 && index == 15) continue;
					size_t length = round ? (1U << field) : sizeof(extra[field]);
					zassert_ok(parse_info(&peers[port], index, LUMP_INFO_UNK7 + field,
						extra[field], length));
				}
			}
			for (size_t port = 0; port < ARRAY_SIZE(peers); port++) {
				zassert_ok(parse_info(&peers[port], index, LUMP_INFO_FORMAT,
					format, sizeof(format)));
			}
		}
		for (size_t port = 0; port < ARRAY_SIZE(peers); port++) {
			zassert_ok(lump_receive(&peers[port], LUMP_SYS_ACK));
			zassert_equal(peers[port].phase, LUMP_ACK);
			for (uint8_t index = 0; index < 16; index++) {
				struct bp_port_mode mode;
				lump_mode_get(&peers[port].modes[index], &mode);
				zassert_equal(mode.num_combinations, 16);
				zassert_equal(mode.combinations[0], 1);
				zassert_equal(mode.combinations[15], 0x3412);
				for (size_t field = 0; field < ARRAY_SIZE(extra); field++) {
					size_t length = round ? (1U << field) : sizeof(extra[field]);
					if (round && field == 2 && index == 15) length = 0;
					zassert_equal(mode.extra_length[field], length);
					zassert_mem_equal(mode.extra[field], extra[field], length);
					for (size_t tail = length; tail < sizeof(mode.extra[field]); tail++) {
						zassert_equal(mode.extra[field][tail], 0);
					}
				}
			}
		}
		printk("%zu 16-mode peers, round %zu: %zu allocated bytes\n", ARRAY_SIZE(peers),
			round, allocated_bytes() - before);
		for (size_t port = 0; port < ARRAY_SIZE(peers); port++) lump_reset(&peers[port]);
		zassert_equal(allocated_bytes(), before, "Disconnected peers retained metadata");
	}
#ifdef UART_CAPACITY_ONLY
	for (uint32_t port = 0; port < ARRAY_SIZE(peers); port++) port_uart_stop(port);
	zassert_equal(allocated_bytes(), idle, "Stopped links retained memory");
#endif
}

ZTEST(lump, test_mode_payload_allocation_failure)
{
	size_t before = allocated_bytes();
	/* Keep one payload live, exhaust the real shared heap, and force the next
	 * INFO insertion to grow it. A failed realloc must leave that payload
	 * intact; reset must then permit a fresh negotiation. */
	static struct lump peer;
	uint8_t type[34], modes[34], id = 201, count = 0;
	const uint8_t name[] = {'N', 0};
	const uint8_t first[] = {0xa5};
	const uint8_t second[32] = {0x31, 0x72, 0xb4, 0xd8};
	const uint8_t format[] = {1, LUMP_DATA_TYPE_DATA8, 3, 0};
	int type_size = lump_command(type, LUMP_CMD_TYPE, &id, 1);
	int modes_size = lump_command(modes, LUMP_CMD_MODES, &count, 1);
	lump_reset(&peer);
	zassert_ok(parse_bytes(&peer, type, type_size));
	zassert_ok(parse_bytes(&peer, modes, modes_size));
	zassert_ok(parse_info(&peer, 0, LUMP_INFO_NAME, name, sizeof(name)));
	zassert_ok(parse_info(&peer, 0, LUMP_INFO_UNK7, first, sizeof(first)));
	void *guard = k_malloc(1);
	zassert_not_null(guard);
	void *held[64];
	size_t held_count = 0;
	for (size_t size = 16384; size; size >>= 1) {
		void *block;
		while ((block = k_malloc(size)) != NULL) {
			zassert_true(held_count < ARRAY_SIZE(held), "Heap exhaustion exceeded fixture bound");
			held[held_count++] = block;
		}
	}
	zassert_is_null(k_malloc(1));
	zassert_equal(parse_info(&peer, 0, LUMP_INFO_UNK8, second, sizeof(second)), -ENOMEM);
	struct bp_port_mode mode;
	lump_mode_get(&peer.modes[0], &mode);
	zassert_equal(mode.extra_length[0], 1);
	zassert_equal(mode.extra[0][0], first[0]);
	zassert_equal(mode.extra_length[1], 0);
	lump_reset(&peer);
	k_free(guard);
	for (size_t i = 0; i < held_count; i++) k_free(held[i]);
	zassert_ok(parse_bytes(&peer, type, type_size));
	zassert_ok(parse_bytes(&peer, modes, modes_size));
	zassert_ok(parse_info(&peer, 0, LUMP_INFO_NAME, name, sizeof(name)));
	zassert_ok(parse_info(&peer, 0, LUMP_INFO_UNK8, second, sizeof(second)));
	zassert_ok(parse_info(&peer, 0, LUMP_INFO_FORMAT, format, sizeof(format)));
	zassert_ok(lump_receive(&peer, LUMP_SYS_ACK));
	lump_mode_get(&peer.modes[0], &mode);
	zassert_equal(mode.extra_length[0], 0);
	zassert_equal(mode.extra_length[1], sizeof(second));
	zassert_mem_equal(mode.extra[1], second, sizeof(second));
	lump_reset(&peer);
	zassert_equal(allocated_bytes(), before, "Failed growth retained metadata");
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

static struct bp_sensor_sample sensor(void)
{
	struct bp_sensor_sample value;
	zassert_ok(hub_sensor_get(0, &value));
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
		if (v.phase == phase) {
			return v;
		}
		zassert_true(k_uptime_get() < deadline, "phase=%u error=%d, wanted=%u", v.phase, v.error, phase);
		k_msleep(1);
	} while (true);
}

static void wait_select(uint8_t mode, size_t from);

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
	/* SPIKE motor startup: ACK, BREAK, then a valid TYPE. Framing and BREAK
	 * are synchronization input here, but remain fatal after negotiation. */
	uart_emul_set_errors(uart, UART_ERROR_FRAMING | UART_BREAK);
	uint8_t sync = LUMP_SYS_SYNC;
	feed(&sync, 1);
	feed(boost_interactive_motor, sizeof(boost_interactive_motor));
	wait_tx(sizeof(speed) + 2);
	zassert_equal(tx[sizeof(speed)], LUMP_SYS_ACK);
	zassert_equal(tx[sizeof(speed) + 1], LUMP_SYS_NACK);
	zassert_ok(uart_config_get(uart, &cfg));
	zassert_equal(cfg.baudrate, 115200);
	zassert_equal(wait_phase(BP_PORT_READY).error, -EAGAIN);
	struct bp_port_mode mode;
	zassert_ok(hub_port_get_mode(0, snapshot().generation, 2, &mode));
	zassert_mem_equal(mode.name, "POS", 4);
	zassert_equal(mode.num_values, 1);
	zassert_equal(mode.data_type, LUMP_DATA_TYPE_DATA32);
	wait_select(2, 0);
	feed(position, sizeof(position));
	zassert_ok(snapshot().error);
	zassert_equal(snapshot().type_id, 38);
	zassert_equal(snapshot().data_sequence, 1);
	zassert_equal(snapshot().mode, 2);
	zassert_equal(snapshot().length, 4);
	zassert_mem_equal(snapshot().data, position + 1, 4);
}

static void smp_snapshot(const char *field, int64_t expected)
{
	/* The registered observation handler must fit the existing transport and
	 * return the exact binary sample seen through the native service. */
	static struct cbor_nb_reader reader;
	static struct cbor_nb_writer writer;
	static uint8_t response[CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE - 8];
	uint8_t request[32];
	const char *name = field ? "sensor.A" : "port.A";
	zcbor_new_encode_state(writer.zs, ARRAY_SIZE(writer.zs), request, sizeof(request), 1);
	zassert_true(zcbor_map_start_encode(writer.zs, 1));
	zassert_true(zcbor_tstr_put_lit(writer.zs, "source"));
	zassert_true(zcbor_tstr_encode_ptr(writer.zs, name, strlen(name)));
	zassert_true(zcbor_map_end_encode(writer.zs, 1));
	zcbor_new_decode_state(reader.zs, ARRAY_SIZE(reader.zs), request,
		writer.zs[0].payload - request, 1, NULL, 0);
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
			if (field && key.len == strlen(field) && !memcmp(key.value, field, key.len)) {
				int64_t value;
				zassert_true(zcbor_int64_decode(reader.zs, &value));
				zassert_equal(value, expected);
				found = true;
			} else if (!field && key.len == 4 && !memcmp(key.value, "data", 4)) {
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

/* Synthetic sensor peer. Metadata formats follow lego/device.h; these are
 * protocol fixtures, not captures from a SPIKE/WeDo sensor. Values below check
 * meaningful units and boundaries rather than duplicate conversion formulas. */
static void packet(uint8_t type, uint8_t mode, uint8_t info, const uint8_t *data, size_t size)
{
	uint8_t frame[35];
	feed(frame, make_packet(frame, type, mode, info, data, size));
}

static void begin_sensor(uint8_t id, size_t count)
{
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
	tx_count = 0;
	pup_test_circuits[0].kind = UART_DEVICE;
	wait_tx(sizeof(speed));
	uint8_t ack = LUMP_SYS_ACK;
	feed(&ack, 1);
	packet(LUMP_MSG_TYPE_CMD, LUMP_CMD_TYPE, 0, &id, 1);
	uint8_t modes[] = {MIN(count - 1, 7), 0, count - 1, 0};
	packet(LUMP_MSG_TYPE_CMD, LUMP_CMD_MODES, 0, modes, 4);
	feed(speed, sizeof(speed));
}

static void connect_sensor(uint8_t id, const uint8_t formats[][2], size_t count, uint8_t flags)
{
	begin_sensor(id, count);
	for (uint8_t mode = 0; mode < count; mode++) {
		uint8_t name[16] = {'S', 'E', 'N', 0, 0, 0, flags};
		packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_NAME, name, sizeof(name));
		uint8_t format[] = {formats[mode][0], formats[mode][1], 4, 0};
		packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_FORMAT, format, sizeof(format));
	}
	uint8_t ack = LUMP_SYS_ACK;
	feed(&ack, 1);
	k_msleep(20);
}

static void inspect_device(void)
{
	/* A synthetic private ID outside Pybricks' old 29..101 discovery filter.
	 * Every numeric payload below is an independently chosen wire value. */
	begin_sensor(201, 4);
	struct bp_port_mode mode;
	zassert_equal(hub_port_get_mode(0, snapshot().generation, 0, &mode), -EAGAIN);
	const uint8_t version[] = {0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0x10};
	packet(LUMP_MSG_TYPE_CMD, LUMP_CMD_VERSION, 0, version, sizeof(version));
	const uint8_t types[] = {1, 0, 2, 3};
	for (uint8_t index = 0; index < 4; index++) {
		const uint8_t name[] = {'X', 0x1b, '!', 0, 0, 0, 0xc0, 0x40, 3, 4, 5, 6, 0, 0, 0, 0};
		const uint8_t plain[] = {'N', 'U', 'M', 0};
		packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_NAME, index ? plain : name,
			index ? sizeof(plain) : sizeof(name));
		if (!index) {
			const uint8_t raw[] = {0, 0, 0, 0xc7, 0, 0xfe, 0xff, 0x46};
			const uint8_t percent[] = {0, 0, 0, 0, 0, 0, 0xc8, 0x42};
			const uint8_t si[] = {0, 0, 0x20, 0xc2, 0, 0, 0xfa, 0x42};
			const uint8_t unit[] = {'d', 'e', 'g', 'C', 0};
			const uint8_t mapping[] = {0x10, 0};
			const uint8_t combinations[] = {3, 0, 12, 0};
			packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_RAW, raw, sizeof(raw));
			packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_PCT, percent, sizeof(percent));
			packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_SI, si, sizeof(si));
			packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_UNITS, unit, sizeof(unit));
			packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_MAPPING, mapping, sizeof(mapping));
			packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_MODE_COMBOS, combinations, sizeof(combinations));
			packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_UNK9, combinations, sizeof(combinations));
			packet(LUMP_MSG_TYPE_INFO, index, 0x0d, combinations, sizeof(combinations));
		}
		const uint8_t format[] = {2, types[index], 6, 2};
		packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_FORMAT, format, sizeof(format));
	}
	uint8_t ack = LUMP_SYS_ACK;
	feed(&ack, 1);
	struct bp_port_state v = wait_phase(BP_PORT_READY);
	zassert_equal(v.error, -EAGAIN); /* Metadata is useful before the first DATA. */
	zassert_equal(v.type_id, 201);
	zassert_true(v.has_version);
	zassert_equal(v.firmware_version, 0x12345678);
	zassert_equal(v.hardware_version, 0x10000000);
	zassert_equal(v.num_view_modes, 1);
	zassert_ok(hub_port_get_mode(0, v.generation, 0, &mode));
	zassert_equal(mode.raw[0], -32768);
	zassert_equal(mode.raw[1], 32767);
	zassert_equal(mode.percent[1], 100);
	zassert_equal(mode.si[0], -40);
	zassert_equal(mode.si[1], 125);
	zassert_mem_equal(mode.unit, "degC", 5);
	zassert_equal(mode.flags[5], 6);
	zassert_equal(mode.num_combinations, 2);
	zassert_equal(mode.combinations[1], 12);
	zassert_equal(mode.extra_length[2], 4);
	zassert_mem_equal(mode.extra[2], ((uint8_t[]){3, 0, 12, 0}), 4);
	zassert_equal(mode.other_info, 1);
	zassert_ok(hub_port_get_mode(0, v.generation, 1, &mode));
	zassert_equal(mode.present, 0, "Missing optional metadata was fabricated");
	zassert_equal(hub_port_get_mode(0, v.generation, 4, &mode), -EINVAL);
	zassert_equal(hub_port_get_mode(0, v.generation - 1, 0, &mode), -ESTALE);

	const struct shell *shell = shell_backend_dummy_get_ptr();
	const uint8_t values[][8] = {
		{0x85, 0xff, 0xff, 0x7f}, {0x80, 0x7f},
		{0, 0, 0, 0x80, 0xff, 0xff, 0xff, 0x7f},
		{0, 0, 0xa0, 0x3f, 0, 0, 0, 0xbf},
	};
	const size_t lengths[] = {4, 2, 8, 8};
	const char *expected[] = {"int16, no scaling): -123 32767", "int8, no scaling): -128 127",
		"int32, no scaling): -2147483648 2147483647", "float32, no scaling): 1.25 -0.5"};
	for (uint8_t index = 0; index < 4; index++) {
		packet(LUMP_MSG_TYPE_DATA, index, 0, values[index], lengths[index]);
		size_t sent_before = tx_count;
		shell_backend_dummy_clear_output(shell);
		zassert_ok(shell_execute_cmd(shell, "port A"));
		size_t length;
		const char *output = shell_backend_dummy_get_output(shell, &length);
		zassert_not_null(strstr(output, "reported device ID=201"));
		zassert_not_null(strstr(output, "firmware=0x12345678 hardware=0x10000000"));
		zassert_not_null(strstr(output, "Native measurements: unavailable"));
		zassert_not_null(strstr(output, "\"X\\x1b!\""));
		zassert_is_null(strchr(output, 0x1b), "Device text injected an ANSI sequence");
		zassert_not_null(strstr(output, "Raw range: -32768 .. 32767"));
		zassert_not_null(strstr(output, "SI range: -40 .. 125"));
		zassert_not_null(strstr(output, "Unit: not advertised"));
		zassert_not_null(strstr(output, "Flags: c0 40 03 04 05 06"));
		zassert_not_null(strstr(output, "Combination masks: 0003 000c"));
		zassert_not_null(strstr(output, "INFO 0x09 (uninterpreted):"));
		zassert_not_null(strstr(output, "03 00 0c 00"));
		zassert_not_null(strstr(output, "Undefined INFO messages: 1"));
		zassert_not_null(strstr(output, expected[index]), "%s", output);
		zassert_equal(tx_count, sent_before, "Inspection initiated device I/O");
		zassert_equal(sensor().owner, BP_PORT_UNOWNED);
		zassert_equal(pup_test_sensor_supply(0), 0, "Unknown flags authorized H-bridge supply");
	}
	zassert_equal(hub_motor_drive(BP_PORT_TERMINAL, 0, 100, 1), -ENOTSUP);
	zassert_equal(shell_execute_cmd(shell, "port G"), -EINVAL);
	shell_backend_dummy_clear_output(shell);
	zassert_ok(shell_execute_cmd(shell, "port"));
	size_t length;
	const char *output = shell_backend_dummy_get_output(shell, &length);
	zassert_not_null(strstr(output, "A: ready; ID=201"));
	zassert_not_null(strstr(output, "F: empty; ID=0"));

	/* Reconnect with no versions/ranges. No old optional information may leak
	 * into another device. Malformed optional metadata invalidates negotiation. */
	begin_sensor(201, 1);
	zassert_equal(hub_port_get_mode(0, v.generation, 0, &mode), -ESTALE);
	const uint8_t plain[] = {'N', 'E', 'W', 0};
	packet(LUMP_MSG_TYPE_INFO, 0, LUMP_INFO_NAME, plain, sizeof(plain));
	const uint8_t bad_range[] = {0, 1};
	packet(LUMP_MSG_TYPE_INFO, 0, LUMP_INFO_RAW, bad_range, sizeof(bad_range));
	zassert_equal(snapshot().error, -EBADMSG);
	zassert_equal(snapshot().phase, BP_PORT_FAULT);
	zassert_false(snapshot().has_version);
	begin_sensor(201, 1);
	packet(LUMP_MSG_TYPE_INFO, 0, LUMP_INFO_NAME, plain, sizeof(plain));
	const uint8_t long_unit[] = {'a', 'b', 'c', 'd', 'e', 0, 0, 0};
	packet(LUMP_MSG_TYPE_INFO, 0, LUMP_INFO_UNITS, long_unit, sizeof(long_unit));
	zassert_equal(snapshot().error, -EBADMSG);
	begin_sensor(201, 1);
	packet(LUMP_MSG_TYPE_INFO, 0, LUMP_INFO_NAME, plain, sizeof(plain));
	uint8_t extra[32];
	for (size_t i = 0; i < sizeof(extra); i++) extra[i] = i;
	packet(LUMP_MSG_TYPE_INFO, 0, LUMP_INFO_UNK12, extra, sizeof(extra));
	packet(LUMP_MSG_TYPE_INFO, 0, LUMP_INFO_UNK12, extra, sizeof(extra));
	zassert_equal(snapshot().error, -EBADMSG, "Duplicate optional metadata accepted");
	const uint8_t formats[][2] = {{1, 0}};
	connect_sensor(201, formats, 1, 0);
	v = snapshot();
	zassert_false(v.has_version);
	zassert_ok(hub_port_get_mode(0, v.generation, 0, &mode));
	zassert_false(mode.present & (BP_PORT_MODE_RAW | BP_PORT_MODE_SI));
	zassert_equal(mode.other_info, 0);
	zassert_equal(mode.extra_length[2], 0);
	/* Highest extended mode and largest opaque payload must survive unchanged. */
	begin_sensor(201, 16);
	for (uint8_t index = 0; index < 16; index++) {
		packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_NAME, plain, sizeof(plain));
		if (index == 15) packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_UNK12, extra, sizeof(extra));
		const uint8_t format[] = {1, 0, 3, 0};
		packet(LUMP_MSG_TYPE_INFO, index, LUMP_INFO_FORMAT, format, sizeof(format));
	}
	feed(&ack, 1);
	v = wait_phase(BP_PORT_READY);
	zassert_equal(v.num_modes, 16);
	zassert_ok(hub_port_get_mode(0, v.generation, 15, &mode));
	zassert_equal(mode.extra_length[5], sizeof(extra));
	zassert_mem_equal(mode.extra[5], extra, sizeof(extra));
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
}

static void wait_select(uint8_t mode, size_t from)
{
	int64_t deadline = k_uptime_get() + 200;
	for (;;) {
		for (size_t i = from; i + 2 < tx_count; i++) {
			if (tx[i] == 0x43 && tx[i + 1] == mode && tx[i + 2] == (0xbc ^ mode)) return;
		}
		zassert_true(k_uptime_get() < deadline, "Missing SELECT %u", mode);
		k_msleep(1);
	}
}

static bool tx_has(const uint8_t *bytes, size_t size, size_t from)
{
	for (size_t i = from; i + size <= tx_count; i++) {
		if (!memcmp(tx + i, bytes, size)) return true;
	}
	return false;
}

static void matrix_first_write(void)
{
	begin_sensor(LEGO_DEVICE_TYPE_ID_TECHNIC_COLOR_LIGHT_MATRIX, 3);
	for (uint8_t mode = 0; mode < 3; mode++) {
		const uint8_t name[] = {'P', 'I', 'X', 0};
		packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_NAME, name, sizeof(name));
		if (mode == LEGO_DEVICE_MODE_PUP_COLOR_LIGHT_MATRIX__PIX_O) {
			const uint8_t mapping[] = {0, 1};
			packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_MAPPING, mapping, sizeof(mapping));
		}
		const uint8_t format[] = {mode == LEGO_DEVICE_MODE_PUP_COLOR_LIGHT_MATRIX__PIX_O ? 9 : 1,
			LUMP_DATA_TYPE_DATA8, 4, 0};
		packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_FORMAT, format, sizeof(format));
	}
	uint8_t ack = LUMP_SYS_ACK;
	feed(&ack, 1);
	struct bp_port_state v = wait_phase(BP_PORT_READY);
	zassert_equal(v.data_sequence, 0);
	size_t from = tx_count;
	uint32_t token;
	zassert_ok(hub_port_matrix_write(BP_PORT_APPLICATION, 0, v.generation, NULL, &token));
	wait_select(LEGO_DEVICE_MODE_PUP_COLOR_LIGHT_MATRIX__PIX_O, from);
	const uint8_t pixel_header[] = {0x46, 0, 0xb9, 0xe2};
	const uint8_t blank[9] = {0};
	packet(LUMP_MSG_TYPE_DATA, LEGO_DEVICE_MODE_PUP_COLOR_LIGHT_MATRIX__PIX_O, 0,
		blank, sizeof(blank));
	zassert_equal(snapshot().data_sequence, 1);
	k_msleep(10);
	zassert_false(tx_has(pixel_header, sizeof(pixel_header), from),
		"First matrix DATA sample allowed an early pixel write");
	zassert_not_equal(snapshot().write.completed, token);
	packet(LUMP_MSG_TYPE_DATA, LEGO_DEVICE_MODE_PUP_COLOR_LIGHT_MATRIX__PIX_O, 0,
		blank, sizeof(blank));
	int64_t deadline = k_uptime_get() + 100;
	while (!tx_has(pixel_header, sizeof(pixel_header), from)) {
		zassert_true(k_uptime_get() < deadline, "Second sample did not release matrix write");
		k_msleep(1);
	}
	while (snapshot().write.completed != token) {
		zassert_true(k_uptime_get() < deadline, "Matrix write did not complete");
		k_msleep(1);
	}
	zassert_ok(snapshot().write.error);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
}

static void writable_sensor(void)
{
	/* Private synthetic device: mode 8 accepts three logical DATA8 bytes.
	 * Its fourth wire byte must be padding, not caller data. */
	begin_sensor(201, 9);
	const uint8_t name[] = {'W', 0};
	for (uint8_t mode = 0; mode < 9; mode++) {
		packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_NAME, name, sizeof(name));
		if (mode == 8) {
			const uint8_t mapping[] = {0, 1};
			packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_MAPPING, mapping, sizeof(mapping));
		}
		const uint8_t format[] = {mode == 8 ? 3 : 1, LUMP_DATA_TYPE_DATA8, 4, 0};
		packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_FORMAT, format, sizeof(format));
	}
	uint8_t ack = LUMP_SYS_ACK;
	feed(&ack, 1);
	struct bp_port_state v = wait_phase(BP_PORT_READY);
	uint32_t token = 0xdeadbeef;
	const uint8_t one = 1;
	zassert_equal(hub_port_write(BP_PORT_APPLICATION, 0, v.generation, 0, &one, 1, &token), -ENOTSUP);
	zassert_equal(hub_port_write(BP_PORT_APPLICATION, 0, v.generation, 9, &one, 1, &token), -EINVAL);
	zassert_equal(hub_port_write(BP_PORT_APPLICATION, 0, v.generation, 8, &one, 1, &token), -EMSGSIZE);
	zassert_equal(hub_port_write(BP_PORT_APPLICATION, 0, v.generation - 1, 8, &one, 1, &token), -ESTALE);
	zassert_equal(token, 0xdeadbeef);
	zassert_equal(sensor().owner, BP_PORT_UNOWNED);
	zassert_equal(snapshot().selected_mode, UINT8_MAX);

	size_t from = tx_count;
	uint8_t values[] = {0x12, 0x34, 0x56};
	zassert_ok(hub_port_write(BP_PORT_APPLICATION, 0, v.generation, 8, values, sizeof(values), &token));
	zassert_not_equal(token, 0);
	memset(values, 0, sizeof(values)); /* The request owns a copy. */
	zassert_equal(snapshot().write.request, token);
	zassert_not_equal(snapshot().write.completed, token);
	zassert_equal(snapshot().write.error, -EAGAIN);
	uint32_t rejected = 0xdeadbeef;
	zassert_equal(hub_port_write(BP_PORT_APPLICATION, 0, v.generation, 8, values, sizeof(values),
		&rejected), -EBUSY);
	zassert_equal(hub_port_write(BP_PORT_TERMINAL, 0, v.generation, 8, values, sizeof(values),
		&rejected), -EBUSY);
	zassert_equal(rejected, 0xdeadbeef);
	zassert_equal(snapshot().write.request, token);
	wait_select(8, from);
	k_msleep(5);
	/* A wrong-mode sample must not authorize the pending output. */
	const uint8_t wrong[] = {0xc0, 0, 0x3f};
	feed(wrong, sizeof(wrong));
	const uint8_t expected[] = {0x46, 8, 0xb1, 0xd0, 0x12, 0x34, 0x56, 0, 0x5f};
	zassert_false(tx_has(expected, sizeof(expected), from));
	const uint8_t selected[] = {0x46, 8, 0xb1, 0xd0, 0xaa, 0xbb, 0xcc, 0, 0xf2};
	zassert_equal(uart_emul_put_rx_data(uart, selected, sizeof(selected)), sizeof(selected));
	int64_t deadline = k_uptime_get() + 200;
	while (!tx_has(expected, sizeof(expected), from)) {
		zassert_true(k_uptime_get() < deadline, "Missing extended DATA output");
		k_msleep(1);
	}
	zassert_not_equal(snapshot().write.completed, token, "Write completed before settling");
	deadline = k_uptime_get() + 200;
	while (snapshot().write.completed != token) {
		zassert_true(k_uptime_get() < deadline, "Write did not complete");
		k_msleep(1);
	}
	zassert_ok(snapshot().write.error);

	/* Change to another mode, then cancel a write before any matching DATA
	 * can arrive. Release must leave its explicit result and send no DATA. */
	from = tx_count;
	zassert_ok(hub_port_select(BP_PORT_APPLICATION, 0, v.generation, 0));
	wait_select(0, from);
	k_msleep(5);
	const uint8_t mode_zero[] = {0x46, 0, 0xb9, 0xc0, 0, 0x3f};
	zassert_equal(uart_emul_put_rx_data(uart, mode_zero, sizeof(mode_zero)), sizeof(mode_zero));
	deadline = k_uptime_get() + 200;
	while (snapshot().mode != 0 || snapshot().mode_error) {
		zassert_true(k_uptime_get() < deadline, "Mode 0 did not become ready");
		k_msleep(1);
	}
	from = tx_count;
	zassert_ok(hub_port_write(BP_PORT_APPLICATION, 0, v.generation, 8, values, sizeof(values), &token));
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	v = snapshot();
	zassert_equal(v.write.completed, token);
	zassert_equal(v.write.error, -ECANCELED);
	zassert_equal(sensor().owner, BP_PORT_UNOWNED);
	const uint8_t cancelled[] = {0x46, 8, 0xb1, 0xd0, 0, 0, 0, 0, 0x2f};
	zassert_false(tx_has(cancelled, sizeof(cancelled), from), "Cancelled DATA reached UART");

	/* Reclaim while the peer is still connected. A release after DATA enters
	 * UART must wait for that transmission and its settling result. SELECT
	 * may already have been sent before the preceding DATA was cancelled. */
	values[0] = 1;
	values[1] = 2;
	values[2] = 3;
	zassert_ok(hub_port_write(BP_PORT_APPLICATION, 0, v.generation, 8, values, sizeof(values), &token));
	wait_select(8, from);
	k_msleep(5);
	zassert_equal(uart_emul_put_rx_data(uart, selected, sizeof(selected)), sizeof(selected));
	const uint8_t in_flight[] = {0x46, 8, 0xb1, 0xd0, 1, 2, 3, 0, 0x2f};
	deadline = k_uptime_get() + 200;
	while (!tx_has(in_flight, sizeof(in_flight), from)) {
		zassert_true(k_uptime_get() < deadline, "Missing in-flight DATA output");
		k_msleep(1);
	}
	zassert_not_equal(snapshot().write.completed, token);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	v = snapshot();
	zassert_equal(v.write.completed, token);
	zassert_equal(v.write.error, -ECANCELED);
	zassert_equal(sensor().owner, BP_PORT_UNOWNED);
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
}

static void words(uint8_t mode, const int16_t *values, size_t count)
{
	uint8_t bytes[32];
	for (size_t i = 0; i < count; i++) sys_put_le16(values[i], bytes + i * 2);
	packet(LUMP_MSG_TYPE_DATA, mode, 0, bytes, count * 2);
}
#define WORDS(mode, ...) do { const int16_t values_[] = {__VA_ARGS__}; \
	words(mode, values_, ARRAY_SIZE(values_)); } while (0)

static void sensor_measurements(void)
{
	const uint8_t color[][2] = {{1, 0}, {1, 0}, {1, 0}, {3, 0}, {2, 1},
		{4, 1}, {3, 1}, {4, 1}, {2, 1}, {7, 1}};
	connect_sensor(61, color, ARRAY_SIZE(color), 0);
	zassert_equal(snapshot().phase, BP_PORT_READY);
	wait_select(5, 0);
	zassert_equal(sensor().error, -EAGAIN);
	WORDS(5, 1024, 0, 0, 0);
	zassert_equal(sensor().error, -EAGAIN, "Unsettled color accepted");
	k_msleep(40);
	WORDS(5, 1024, 0, 0, 0);
	struct bp_sensor_sample v = sensor();
	zassert_ok(v.error);
	zassert_equal(v.hue_deg, 10);
	zassert_equal(v.saturation_pct, 100);
	zassert_equal(v.value_pct, 100);
	zassert_equal(v.reflection_permille, 333);
	zassert_equal(v.fields, BP_SENSOR_HAS_HSV | BP_SENSOR_HAS_REFLECTION | BP_SENSOR_HAS_MATCHED_COLOR);
	zassert_equal(v.matched_color, 0x00006464); /* Default palette: red (0, 100, 100). */
	zassert_equal(pup_test_sensor_supply(0), 0);
	uint32_t sequence = v.sequence;
	int64_t sampled = v.sample_ms;
	WORDS(5, -1, 0, 0, 0);
	zassert_equal(sensor().error, -EBADMSG);
	zassert_equal(sensor().fields, 0);
	zassert_equal(sensor().sample_ms, sampled);
	zassert_equal(sensor().sequence, sequence);
	WORDS(5, 1024, 1024, 1024, 0);
	zassert_ok(sensor().error);
	zassert_equal(sensor().reflection_permille, 1000);
	zassert_equal(sensor().saturation_pct, 0);
	smp_snapshot("reflection_permille", 1000);
	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(shell_execute_cmd(shell, "observe sensor.A"));
	size_t length;
	const char *output = shell_backend_dummy_get_output(shell, &length);
	zassert_not_null(strstr(output, "reflection_permille=1000"));
	zassert_ok(hub_sensor_configure(BP_PORT_APPLICATION, 0, BP_SENSOR_SURFACE));
	zassert_equal(shell_execute_cmd(shell, "sensor A ambient"), -EBUSY);
	length = tx_count;
	zassert_equal(hub_sensor_configure(BP_PORT_APPLICATION, 0, BP_SENSOR_FORCE), -ENOTSUP);
	zassert_equal(sensor().owner, BP_PORT_APPLICATION);
	zassert_equal(tx_count, length, "Unsupported request wrote UART");
	zassert_ok(hub_sensor_configure(BP_PORT_APPLICATION, 0, BP_SENSOR_AMBIENT));
	zassert_equal(sensor().error, -EAGAIN);
	wait_select(7, length);
	WORDS(5, 1024, 1024, 1024, 0);
	zassert_equal(sensor().error, -EAGAIN, "Wrong mode masqueraded as ambient");
	WORDS(7, 120, 500, 400, 0);
	zassert_equal(sensor().error, -EAGAIN);
	k_msleep(40);
	WORDS(7, 120, 500, 400, 0);
	zassert_ok(sensor().error);
	zassert_equal(sensor().hue_deg, 120);
	zassert_equal(sensor().saturation_pct, 50);
	zassert_equal(sensor().ambient_permille, 40);
	/* A recognized sensor can still expose modes outside normalized sensing.
	 * Reject invalid claims before changing ownership or sending SELECT. */
	uint32_t generation = snapshot().generation;
	length = tx_count;
	zassert_equal(hub_port_select(BP_PORT_TERMINAL, 0, generation, 0), -EBUSY);
	zassert_equal(hub_port_select(BP_PORT_APPLICATION, 0, generation - 1, 0), -ESTALE);
	zassert_equal(hub_port_select(BP_PORT_APPLICATION, 0, generation, ARRAY_SIZE(color)), -EINVAL);
	zassert_equal(tx_count, length, "Rejected raw selection wrote UART");
	zassert_equal(snapshot().selected_mode, UINT8_MAX);
	zassert_equal(sensor().owner, BP_PORT_APPLICATION);
	zassert_ok(hub_port_select(BP_PORT_APPLICATION, 0, generation, 0));
	zassert_equal(snapshot().selected_mode, 0);
	zassert_equal(snapshot().mode_error, -EAGAIN);
	zassert_equal(sensor().error, -ENOTSUP);
	wait_select(0, length);
	WORDS(7, 120, 500, 400, 0);
	zassert_equal(snapshot().mode_error, -EAGAIN, "Wrong mode completed raw selection");
	uint8_t raw = 0x5a;
	packet(LUMP_MSG_TYPE_DATA, 0, 0, &raw, 1);
	zassert_equal(snapshot().mode_error, -EAGAIN, "Unsettled raw data became ready");
	k_msleep(40);
	packet(LUMP_MSG_TYPE_DATA, 0, 0, &raw, 1);
	struct bp_port_state selected = snapshot();
	zassert_ok(selected.mode_error);
	zassert_equal(selected.selected_mode, 0);
	zassert_equal(selected.mode, 0);
	zassert_equal(selected.length, 1);
	zassert_equal(selected.data[0], raw);
	zassert_equal(sensor().error, -ENOTSUP, "Raw mode published a normalized measurement");
	length = tx_count;
	hub_ports_session_closed(BP_PORT_TERMINAL);
	zassert_equal(sensor().owner, BP_PORT_APPLICATION, "Disconnect stole app ownership");
	zassert_ok(hub_port_release(BP_PORT_APPLICATION, 0));
	zassert_equal(sensor().owner, BP_PORT_UNOWNED);
	zassert_equal(snapshot().selected_mode, UINT8_MAX);
	zassert_equal(snapshot().mode_error, -ENOTSUP);
	wait_select(5, length);
	zassert_ok(shell_execute_cmd(shell, "sensor A ambient"));
	zassert_equal(sensor().owner, BP_PORT_TERMINAL);
	hub_ports_session_closed(BP_PORT_TERMINAL);
	zassert_equal(sensor().owner, BP_PORT_UNOWNED);
	/* A peer can keep the raw link alive while ignoring SELECT. It must not
	 * keep the requested measurement pending indefinitely. */
	int64_t began = k_uptime_get();
	while (snapshot().phase != BP_PORT_FAULT) {
		zassert_true(k_uptime_get() - began < 650, "Unbounded SELECT wait");
		WORDS(7, 120, 500, 400, 0);
	}
	zassert_equal(snapshot().error, -ETIMEDOUT);

	const uint8_t force[][2] = {{1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 1}, {1, 0}, {8, 1}};
	connect_sensor(63, force, ARRAY_SIZE(force), 0);
	wait_select(6, 0);
	WORDS(6, 0, 50, 100, 0, 0, 0, 100, 0);
	zassert_equal(sensor().error, -EBADMSG, "Bad calibration accepted");
	WORDS(6, 0, 50, 100, 0, 0, 0, 1100, 0);
	wait_select(4, 0);
	zassert_equal(sensor().error, -EAGAIN);
	WORDS(4, 600);
	zassert_ok(sensor().error);
	zassert_equal(sensor().force_mn, 4500);
	zassert_equal(sensor().travel_um, 3335);
	smp_snapshot("force_mn", 4500);
	WORDS(4, 50);
	zassert_ok(sensor().error);
	zassert_equal(sensor().force_mn, 0);
	zassert_equal(sensor().travel_um, -333);

	const uint8_t distance[][2] = {{1, 1}, {1, 1}, {1, 1}, {1, 0}};
	connect_sensor(62, distance, ARRAY_SIZE(distance), LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN1);
	wait_select(0, 0);
	zassert_equal(pup_test_sensor_supply(0), 1, "Sensor supply polarity");
	zassert_equal(snapshot().supply, 1);
	WORDS(0, 1234);
	zassert_equal(sensor().error, -EAGAIN, "Unsettled ultrasound accepted");
	k_msleep(60);
	WORDS(0, 1234);
	zassert_ok(sensor().error);
	zassert_equal(sensor().distance_mm, 1234);
	zassert_equal(sensor().fields, BP_SENSOR_HAS_DISTANCE);
	smp_snapshot("distance_mm", 1234);
	WORDS(0, -1);
	zassert_equal(sensor().distance_mm, 2000);
	length = tx_count;
	zassert_ok(shell_execute_cmd(shell, "sensor A presence"));
	wait_select(3, length);
	uint8_t yes = 1;
	packet(LUMP_MSG_TYPE_DATA, 3, 0, &yes, 1);
	k_msleep(60);
	packet(LUMP_MSG_TYPE_DATA, 3, 0, &yes, 1);
	zassert_ok(sensor().error);
	zassert_equal(sensor().presence, 1);
	/* After a valid sample, unrelated DATA must not preserve its validity. */
	pup_test_circuits[0].coast_error = -EACCES;
	began = k_uptime_get();
	while (snapshot().phase != BP_PORT_FAULT) {
		zassert_true(k_uptime_get() - began < 400, "Stale presence remained valid");
		WORDS(0, 1234);
	}
	zassert_equal(sensor().error, -ETIMEDOUT);
	zassert_equal(sensor().fields, 0);
	zassert_equal(snapshot().cleanup_error, -EACCES, "Lost supply cleanup failure");
	zassert_equal(pup_test_sensor_supply(0), 1);

	const uint8_t tilt[][2] = {{2, 0}};
	connect_sensor(34, tilt, ARRAY_SIZE(tilt), 0);
	zassert_equal(pup_test_sensor_supply(0), 0, "Supply survived previous disconnect");
	zassert_equal(sensor().owner, BP_PORT_UNOWNED);
	wait_select(0, 0);
	const uint8_t angles[] = {(uint8_t)-20, 30};
	packet(LUMP_MSG_TYPE_DATA, 0, 0, angles, sizeof(angles));
	zassert_ok(sensor().error);
	zassert_equal(sensor().tilt_deg[0], 30);
	zassert_equal(sensor().tilt_deg[1], -20);
	const uint8_t infrared[][2] = {{1, 0}, {1, 2}, {3, 1}};
	connect_sensor(35, infrared, ARRAY_SIZE(infrared), 0);
	wait_select(2, 0);
	WORDS(2, 100, 0, 0);
	zassert_ok(sensor().error);
	zassert_equal(sensor().reflection_permille, 200);
	zassert_equal(sensor().distance_pct, 10);
	length = tx_count;
	zassert_ok(shell_execute_cmd(shell, "sensor A count"));
	wait_select(1, length);
	const uint8_t count[] = {0xfe, 0xff, 0xff, 0xff};
	packet(LUMP_MSG_TYPE_DATA, 1, 0, count, sizeof(count));
	zassert_ok(sensor().error);
	zassert_equal(sensor().fields, BP_SENSOR_HAS_COUNT);
	zassert_equal(sensor().count, -2);

	const uint8_t boost[][2] = {{1, 0}, {1, 0}, {1, 2}, {1, 0}, {1, 0}, {1, 0}, {3, 1}};
	connect_sensor(37, boost, ARRAY_SIZE(boost), LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN2);
	wait_select(6, 0);
	zassert_equal(pup_test_sensor_supply(0), 2);
	WORDS(6, 900, 900, 900);
	k_msleep(40);
	WORDS(6, 900, 900, 900);
	zassert_ok(sensor().error);
	zassert_equal(sensor().reflection_permille, 1000);
	zassert_equal(sensor().value_pct, 100, "Bright RGB wrapped instead of saturating");
	zassert_equal(sensor().saturation_pct, 0);
	length = tx_count;
	zassert_ok(hub_sensor_configure(BP_PORT_APPLICATION, 0, BP_SENSOR_DISTANCE));
	wait_select(1, length);
	uint8_t proximity = 7;
	packet(LUMP_MSG_TYPE_DATA, 1, 0, &proximity, 1);
	k_msleep(40);
	packet(LUMP_MSG_TYPE_DATA, 1, 0, &proximity, 1);
	zassert_ok(sensor().error);
	zassert_equal(sensor().distance_pct, 70);
	zassert_equal(sensor().fields, BP_SENSOR_HAS_RELATIVE_DISTANCE);
	zassert_equal(hub_motor_coast(BP_PORT_APPLICATION, 0), -ENOTSUP);
	zassert_equal(pup_test_sensor_supply(0), 2, "Motor command changed a sensor supply");

	/* Unsupported formats and contradictory motor flags cannot authorize
	 * sensor supply. */
	connect_sensor(62, tilt, ARRAY_SIZE(tilt), LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN1);
	zassert_equal(sensor().error, -EPROTO);
	zassert_equal(sensor().available, 0);
	zassert_equal(pup_test_sensor_supply(0), 0);
	connect_sensor(62, distance, ARRAY_SIZE(distance),
		LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN1 | LUMP_MODE_FLAGS0_MOTOR);
	zassert_equal(snapshot().phase, BP_PORT_FAULT);
	zassert_equal(snapshot().error, -EPROTO);
	zassert_equal(pup_test_sensor_supply(0), 0);
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
}

static void encoder_measurements(void)
{
	negotiate(true);
	zassert_ok(sensor().error);
	zassert_equal(sensor().fields, BP_SENSOR_HAS_ANGLE);
	zassert_equal(sensor().angle_mdeg, 4660000);
	zassert_equal(sensor().measurement, BP_SENSOR_ANGLE);
	zassert_equal(pup_test_sensor_supply(0), 0);
	/* The device counter crosses both signed limits without a spurious
	 * revolution reset. */
	const int32_t relative[] = {INT32_MAX, INT32_MIN, INT32_MAX, -2};
	const int64_t expected[] = {2147483647000LL, 2147483648000LL, 2147483647000LL, 4294967294000LL};
	for (size_t i = 0; i < ARRAY_SIZE(relative); i++) {
		uint8_t bytes[4];
		sys_put_le32(relative[i], bytes);
		packet(LUMP_MSG_TYPE_DATA, 2, 0, bytes, sizeof(bytes));
		zassert_ok(sensor().error);
		zassert_equal(sensor().angle_mdeg, expected[i], "relative %zu", i);
	}
	smp_snapshot("angle_mdeg", 4294967294000LL);
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
	/* Use the unchanged upstream Technic motor handshake, then independently
	 * constructed angles. The first CALIB word is not the output angle. */
	tx_count = 0;
	pup_test_circuits[0].kind = UART_DEVICE;
	wait_tx(sizeof(speed));
	uint8_t ack = LUMP_SYS_ACK;
	feed(&ack, 1);
	feed(technic_large_motor, sizeof(technic_large_motor));
	wait_select(4, 0);
	const int16_t absolute[] = {3500, 100, 1700, 2500, 3500, 100, 3500, 2500, 1700, 100, 3600};
	const int32_t turns[] = {-10000, 10000, 170000, 250000, 350000, 370000, 350000,
		250000, 170000, 10000, 0};
	for (size_t i = 0; i < ARRAY_SIZE(absolute); i++) {
		WORDS(4, -123, absolute[i]);
		zassert_ok(sensor().error);
		zassert_equal(sensor().angle_mdeg, turns[i], "absolute %zu", i);
		zassert_equal(sensor().fields, BP_SENSOR_HAS_ANGLE | BP_SENSOR_HAS_ABSOLUTE_ANGLE);
		zassert_equal(pup_test_sensor_supply(0), 0);
	}
	struct bp_control_frame frame;
	zassert_ok(control_frame_get(BIT(0), &frame));
	zassert_ok(frame.ports[0].error);
	zassert_equal(frame.ports[0].output_error, -ESTALE);
	zassert_false(frame.ports[0].valid);
	zassert_not_equal(frame.ports[0].output_generation, frame.ports[0].generation);
	WORDS(4, 0, 1800);
	zassert_equal(sensor().absolute_mdeg, -180000);
	smp_snapshot("absolute_mdeg", -180000);
	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(shell_execute_cmd(shell, "sensor A angle"));
	zassert_equal(sensor().owner, BP_PORT_TERMINAL);
	k_msleep(5);
	WORDS(4, 0, 1799);
	shell_backend_dummy_clear_output(shell);
	zassert_ok(shell_execute_cmd(shell, "observe sensor.A"));
	size_t length;
	const char *output = shell_backend_dummy_get_output(shell, &length);
	zassert_not_null(strstr(output, "absolute_mdeg=179900"));
	zassert_equal(hub_motor_drive(BP_PORT_APPLICATION, 0, 300, 0), -EBUSY);
	zassert_ok(shell_execute_cmd(shell, "motor A duty 300"));
	k_msleep(5);
	struct bp_motor_state motor;
	zassert_ok(hub_motor_get(0, &motor));
	zassert_ok(motor.error);
	zassert_true(motor.valid && !motor.pending);
	zassert_equal(motor.duty, 300);
	WORDS(4, 0, 1799);
	zassert_equal(pup_test_sensor_supply(0), 2, "Encoder poll overwrote motor PWM");
	struct bp_sensor_sample before = sensor();
	/* One control read preserves source time and the same port's completed
	 * output, without claiming it or allocating a model. */
	size_t allocated = allocated_bytes();
	zassert_ok(control_frame_get(BIT(0), &frame));
	zassert_ok(frame.ports[0].error);
	zassert_equal(frame.ports[0].generation, before.generation);
	zassert_equal(frame.ports[0].sequence, before.sequence);
	zassert_equal(frame.ports[0].sample_ms, before.sample_ms);
	zassert_equal(frame.ports[0].angle_mdeg, before.angle_mdeg);
	zassert_equal(frame.ports[0].duty_permille, 300);
	zassert_equal(frame.ports[0].completed, motor.completed);
	zassert_equal(frame.ports[0].request_generation, before.generation);
	zassert_equal(frame.ports[0].output_generation, before.generation);
	zassert_true(frame.ports[0].valid && !frame.ports[0].pending);
	zassert_equal(frame.ports[0].owner, BP_PORT_TERMINAL);
	zassert_equal(allocated_bytes(), allocated);
	zassert_true(frame.capture_start_us <= frame.capture_end_us);
	WORDS(4, 0, 3601);
	zassert_equal(sensor().error, -EBADMSG);
	zassert_equal(sensor().fields, 0);
	zassert_equal(sensor().sequence, before.sequence);
	zassert_equal(sensor().sample_ms, before.sample_ms);
	zassert_ok(control_frame_get(BIT(0), &frame));
	zassert_equal(frame.ports[0].error, -EBADMSG);
	zassert_equal(frame.ports[0].sample_ms, before.sample_ms);
	zassert_equal(frame.ports[0].sequence, before.sequence);
	zassert_ok(hub_motor_get(0, &motor));
	zassert_equal(motor.error, -EBADMSG);
	zassert_equal(motor.mode, BP_MOTOR_COAST);
	zassert_equal(pup_test_sensor_supply(0), 0);
	WORDS(4, 0, -1);
	zassert_equal(sensor().error, -EBADMSG);
	WORDS(4, 0, 1801);
	zassert_ok(sensor().error);
	zassert_equal(sensor().angle_mdeg, before.angle_mdeg + 200);
	zassert_equal(sensor().absolute_mdeg, -179900);
	hub_ports_session_closed(BP_PORT_TERMINAL);
	k_msleep(5);
	WORDS(4, 0, 1802);
	zassert_equal(sensor().angle_mdeg, before.angle_mdeg + 300, "Release reset turns");
	zassert_equal(sensor().owner, BP_PORT_UNOWNED);
	zassert_ok(hub_motor_drive(BP_PORT_APPLICATION, 0, -200, 0));
	k_msleep(5);
	zassert_equal(pup_test_sensor_supply(0), 1);
	uint32_t generation = sensor().generation;
	zassert_equal(wait_phase(BP_PORT_FAULT).error, -ETIMEDOUT);
	zassert_ok(control_frame_get(BIT(0), &frame));
	zassert_equal(frame.ports[0].error, -ETIMEDOUT);
	zassert_not_equal(frame.ports[0].generation, generation);
	zassert_equal(pup_test_sensor_supply(0), 0, "UART loss left the motor powered");
	zassert_ok(hub_motor_get(0, &motor));
	zassert_equal(motor.owner, BP_PORT_UNOWNED);
	zassert_equal(sensor().fields, 0);
	const uint8_t formats[][2] = {{1, 0}, {1, 0}, {1, 2}, {1, 1}, {2, 1}, {14, 1}};
	/* Every supported absolute motor shares this format, but capability and
	 * format must both match. Even supply flags without motor flags cannot
	 * accidentally turn encoder selection into a DC output command. */
	const uint8_t ids[] = {46, 47, 48, 49, 65, 75, 76};
	for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
		connect_sensor(ids[i], formats, ARRAY_SIZE(formats),
			LUMP_MODE_FLAGS0_MOTOR_ABS_POS | LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN1 |
			LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN2);
		wait_select(4, 0);
		WORDS(4, 0, 100);
		zassert_ok(sensor().error);
		zassert_equal(sensor().angle_mdeg, 10000, "Reconnect retained turns");
		zassert_true(sensor().generation > generation);
		zassert_equal(snapshot().supply, 0);
		zassert_equal(pup_test_sensor_supply(0), 0);
	}
	connect_sensor(38, formats, ARRAY_SIZE(formats), LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN1);
	wait_select(2, 0);
	zassert_equal(pup_test_sensor_supply(0), 0);
	connect_sensor(46, formats, ARRAY_SIZE(formats), 0);
	zassert_equal(sensor().error, -EPROTO);
	zassert_equal(sensor().available, 0);
	const uint8_t bad_format[][2] = {{1, 0}, {1, 0}, {1, 2}, {1, 1}, {1, 1}};
	connect_sensor(46, bad_format, ARRAY_SIZE(bad_format), LUMP_MODE_FLAGS0_MOTOR_ABS_POS);
	zassert_equal(sensor().error, -EPROTO);
	zassert_equal(sensor().available, 0);
	zassert_equal(pup_test_sensor_supply(0), 0);
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
}

ZTEST(lump, test_uart_hotplug)
{
	uart_emul_callback_tx_data_ready_set(uart, sent, NULL);
	size_t heap_before_uart = allocated_bytes();
	/* A peer that sends SYNC forever must eventually release the port.
	 * Accelerated simulator time makes this independent of wall-clock waits. */
	pup_test_circuits[0].kind = UART_DEVICE;
	wait_tx(sizeof(speed));
	zassert_true(allocated_bytes() > heap_before_uart,
		"UART start did not allocate its live transport");
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
	zassert_equal(allocated_bytes(), heap_before_uart,
		"Stopped UART retained its transport");

	/* Fail a real port start with the shared heap full. The failed attempt
	 * must leave no published device or allocation, then normal probing can
	 * resume after the filler blocks are released. */
	struct heap_filler { struct heap_filler *next; };
	struct heap_filler *fillers = NULL, *block;
	while ((block = k_malloc(64)) != NULL) {
		block->next = fillers;
		fillers = block;
	}
	zassert_not_null(fillers);
	size_t heap_full = allocated_bytes();
	tx_count = 0;
	pup_test_circuits[0].kind = UART_DEVICE;
	struct bp_port_state failed = wait_phase(BP_PORT_FAULT);
	zassert_equal(failed.error, -ENOMEM);
	zassert_equal(failed.length, 0);
	zassert_equal(tx_count, 0);
	zassert_true(pup_test_released(0));
	zassert_equal(allocated_bytes(), heap_full, "Failed UART start leaked memory");
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
	while (fillers) {
		block = fillers;
		fillers = block->next;
		k_free(block);
	}
	zassert_equal(allocated_bytes(), heap_before_uart);
	/* Likewise, no first TYPE at all must not strand electrical detection. */
	tx_count = 0;
	pup_test_circuits[0].kind = UART_DEVICE;
	wait_tx(sizeof(speed));
	zassert_equal(wait_phase(BP_PORT_FAULT).error, -ETIMEDOUT);
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(BP_PORT_EMPTY);
	negotiate(false);
	size_t before_observers = allocated_bytes();
	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(shell_execute_cmd(shell, "observe port.A"));
	size_t size;
	const char *output = shell_backend_dummy_get_output(shell, &size);
	zassert_not_null(strstr(output, "type_id=38"));
	zassert_not_null(strstr(output, "data_sequence=1"));
	smp_snapshot(NULL, 0);
	/* Shell command history has its own registered heap and outlives this
	 * connection. Isolate its net allocation from UART/link reclamation. */
	int64_t observer_delta = (int64_t)allocated_bytes() - (int64_t)before_observers;

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
	zassert_equal((int64_t)allocated_bytes() - (int64_t)heap_before_uart, observer_delta,
		"Negotiated UART retained metadata after unplug");
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
	sensor_measurements();
	encoder_measurements();
	inspect_device();
	writable_sensor();
	matrix_first_write();
	size_t heap_before_shutdown = allocated_bytes();
	negotiate(true);
	zassert_ok(hub_ports_shutdown());
	zassert_equal(allocated_bytes(), heap_before_shutdown,
		"Shutdown retained the active UART transport");
	zassert_equal(snapshot().phase, BP_PORT_OFF);
	zassert_equal(snapshot().length, 0);
	zassert_false(pup_test_supply());
	zassert_true(pup_test_released(0));
	before = snapshot();
	feed(position, sizeof(position));
	zassert_equal(snapshot().sequence, before.sequence);
}

ZTEST_SUITE(lump, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(lump_capacity, NULL, NULL, NULL, NULL, NULL);

#ifdef UART_CAPACITY_ONLY
void test_main(void)
{
	ztest_run_test_suite(lump_capacity, false, 1, 1, NULL);
}
#endif
