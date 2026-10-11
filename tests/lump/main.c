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
#include <zephyr/sys/byteorder.h>
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
	feed(boost_interactive_motor, sizeof(boost_interactive_motor));
	wait_tx(sizeof(speed) + 2);
	zassert_equal(tx[sizeof(speed)], LUMP_SYS_ACK);
	zassert_equal(tx[sizeof(speed) + 1], LUMP_SYS_NACK);
	zassert_ok(uart_config_get(uart, &cfg));
	zassert_equal(cfg.baudrate, 115200);
	zassert_equal(wait_phase(BP_PORT_READY).error, -EAGAIN);
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
	uint8_t frame[35], exponent = 0;
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
	feed(frame, pos);
}

static void connect_sensor(uint8_t id, const uint8_t formats[][2], size_t count, uint8_t flags)
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
	for (uint8_t mode = 0; mode < count; mode++) {
		uint8_t name[16] = {'S', 'E', 'N', 0, 0, 0, flags};
		packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_NAME, name, sizeof(name));
		uint8_t format[] = {formats[mode][0], formats[mode][1], 4, 0};
		packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_FORMAT, format, sizeof(format));
	}
	feed(&ack, 1);
	k_msleep(20);
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

static void words(uint8_t mode, const int16_t *values, size_t count)
{
	uint8_t bytes[32];
	for (size_t i = 0; i < count; i++) sys_put_le16(values[i], bytes + i * 2);
	packet(LUMP_MSG_TYPE_DATA, mode, 0, bytes, count * 2);
}
#define WORDS(mode, ...) do { const int16_t values_[] = {__VA_ARGS__}; \
	words(mode, values_, ARRAY_SIZE(values_)); } while (0)

static struct bp_sensor_sample sensor(void)
{
	struct bp_sensor_sample value;
	zassert_ok(hub_sensor_get(0, &value));
	return value;
}

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
	zassert_equal(v.fields, BP_SENSOR_HAS_HSV | BP_SENSOR_HAS_REFLECTION);
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
	zassert_ok(hub_sensor_configure(BP_SENSOR_APPLICATION, 0, BP_SENSOR_SURFACE));
	zassert_equal(shell_execute_cmd(shell, "sensor A ambient"), -EBUSY);
	length = tx_count;
	zassert_equal(hub_sensor_configure(BP_SENSOR_APPLICATION, 0, BP_SENSOR_FORCE), -ENOTSUP);
	zassert_equal(sensor().owner, BP_SENSOR_APPLICATION);
	zassert_equal(tx_count, length, "Unsupported request wrote UART");
	zassert_ok(hub_sensor_configure(BP_SENSOR_APPLICATION, 0, BP_SENSOR_AMBIENT));
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
	length = tx_count;
	hub_sensors_release(BP_SENSOR_TERMINAL);
	zassert_equal(sensor().owner, BP_SENSOR_APPLICATION, "Disconnect stole app ownership");
	hub_sensors_release(BP_SENSOR_APPLICATION);
	zassert_equal(sensor().owner, BP_SENSOR_UNOWNED);
	wait_select(5, length);
	zassert_ok(shell_execute_cmd(shell, "sensor A ambient"));
	zassert_equal(sensor().owner, BP_SENSOR_TERMINAL);
	hub_sensors_release(BP_SENSOR_TERMINAL);
	zassert_equal(sensor().owner, BP_SENSOR_UNOWNED);
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
	zassert_equal(sensor().owner, BP_SENSOR_UNOWNED);
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
	zassert_ok(hub_sensor_configure(BP_SENSOR_APPLICATION, 0, BP_SENSOR_DISTANCE));
	wait_select(1, length);
	uint8_t proximity = 7;
	packet(LUMP_MSG_TYPE_DATA, 1, 0, &proximity, 1);
	k_msleep(40);
	packet(LUMP_MSG_TYPE_DATA, 1, 0, &proximity, 1);
	zassert_ok(sensor().error);
	zassert_equal(sensor().distance_pct, 70);
	zassert_equal(sensor().fields, BP_SENSOR_HAS_RELATIVE_DISTANCE);

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
	WORDS(4, 0, 1800);
	zassert_equal(sensor().absolute_mdeg, -180000);
	smp_snapshot("absolute_mdeg", -180000);
	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(shell_execute_cmd(shell, "sensor A angle"));
	zassert_equal(sensor().owner, BP_SENSOR_TERMINAL);
	k_msleep(5);
	WORDS(4, 0, 1799);
	shell_backend_dummy_clear_output(shell);
	zassert_ok(shell_execute_cmd(shell, "observe sensor.A"));
	size_t length;
	const char *output = shell_backend_dummy_get_output(shell, &length);
	zassert_not_null(strstr(output, "absolute_mdeg=179900"));
	struct bp_sensor_sample before = sensor();
	WORDS(4, 0, 3601);
	zassert_equal(sensor().error, -EBADMSG);
	zassert_equal(sensor().fields, 0);
	zassert_equal(sensor().sequence, before.sequence);
	zassert_equal(sensor().sample_ms, before.sample_ms);
	WORDS(4, 0, -1);
	zassert_equal(sensor().error, -EBADMSG);
	WORDS(4, 0, 1801);
	zassert_ok(sensor().error);
	zassert_equal(sensor().angle_mdeg, before.angle_mdeg + 200);
	zassert_equal(sensor().absolute_mdeg, -179900);
	hub_sensors_release(BP_SENSOR_TERMINAL);
	k_msleep(5);
	WORDS(4, 0, 1802);
	zassert_equal(sensor().angle_mdeg, before.angle_mdeg + 300, "Release reset turns");
	zassert_equal(sensor().owner, BP_SENSOR_UNOWNED);
	uint32_t generation = sensor().generation;
	zassert_equal(wait_phase(BP_PORT_FAULT).error, -ETIMEDOUT);
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
	smp_snapshot(NULL, 0);

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
	sensor_measurements();
	encoder_measurements();
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
