// SPDX-License-Identifier: Apache-2.0
#include "host_cc2640.h"
#include "host_transport.h"
#include "hub_power.h"
#include "cc2640_npi.h"

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/transport/serial.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#define RADIO DEVICE_GET(mock_npi)

struct frame {
	size_t size;
	uint8_t bytes[CC2640_NPI_MAX_PACKET];
};
K_MSGQ_DEFINE(incoming, sizeof(struct frame), 16, 4);
K_MSGQ_DEFINE(outgoing, sizeof(struct frame), 32, 4);
static K_SEM_DEFINE(advertising_done, 0, 2);
static K_SEM_DEFINE(command_done, 0, 2);
static K_SEM_DEFINE(smp_done, 0, 1);
static K_SEM_DEFINE(smp_block_entered, 0, 1);
static K_SEM_DEFINE(smp_block_release, 0, 1);
static K_SEM_DEFINE(block_entered, 0, 1);
static K_SEM_DEFINE(block_release, 0, 1);
static atomic_ptr_t worker_wake;
static atomic_t unexpected, notifications, retried, adv_updates, advertising_starts;
static uint8_t first_notification[CC2640_NPI_MAX_PACKET];
static size_t first_notification_size;
static atomic_t reject_next_notification;
static atomic_t disconnect_on_marker;
static atomic_t injected_disconnect;
static atomic_t smp_block_calls;
static unsigned int service;
static unsigned int attributes_left;
static uint16_t service_start;
static const uint16_t ranges[][2] = {
	{1, 1}, {2, 8}, {9, 15}, {16, 21}, {22, 27}, {28, 33}, {34, 37},
};
static const uint8_t address[6] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xba};

static int mock_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}
DEVICE_DEFINE(mock_npi, "mock_npi", mock_init, NULL, NULL, NULL,
	      POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);

static void enqueue(const uint8_t *bytes, size_t size)
{
	struct frame frame = {.size = size};
	if (size > sizeof(frame.bytes)) {
		atomic_inc(&unexpected);
		return;
	}
	memcpy(frame.bytes, bytes, size);
	if (k_msgq_put(&incoming, &frame, K_NO_WAIT)) atomic_inc(&unexpected);
	struct k_sem *wake = atomic_ptr_get(&worker_wake);
	if (wake) k_sem_give(wake);
}

static void record(const uint8_t *bytes, size_t size)
{
	struct frame frame = {.size = size};
	if (size > sizeof(frame.bytes)) {
		atomic_inc(&unexpected);
		return;
	}
	memcpy(frame.bytes, bytes, size);
	if (k_msgq_put(&outgoing, &frame, K_NO_WAIT)) atomic_inc(&unexpected);
}

static int command_status(uint16_t opcode, uint8_t status, uint8_t *event)
{
	const uint8_t reply[] = {4, 0xff, 6, 0x7f, 6, status,
		(uint8_t)opcode, (uint8_t)(opcode >> 8), 0};
	memcpy(event, reply, sizeof(reply));
	return sizeof(reply);
}

static void vendor_done(uint16_t code, uint8_t status)
{
	const uint8_t event[] = {4, 0xff, 3, (uint8_t)code,
		(uint8_t)(code >> 8), status};
	enqueue(event, sizeof(event));
}

int cc2640_npi_reset(const struct device *dev)
{
	if (dev != RADIO) return -ENODEV;
	service = attributes_left = 0;
	service_start = 0;
	return 0;
}

int cc2640_npi_off(const struct device *dev)
{
	return dev == RADIO ? 0 : -ENODEV;
}

void cc2640_npi_set_wake(const struct device *dev, struct k_sem *wake)
{
	if (dev != RADIO) {
		atomic_inc(&unexpected);
		return;
	}
	atomic_ptr_set(&worker_wake, wake);
	if (wake) k_sem_give(wake);
}

int cc2640_npi_exchange(const struct device *dev, const uint8_t *command,
			 size_t size, uint8_t *event, k_timeout_t timeout)
{
	if (dev != RADIO || !event) return -EINVAL;
	if (!size) {
		struct frame frame;
		if (k_msgq_get(&incoming, &frame, timeout)) return -EAGAIN;
		memcpy(event, frame.bytes, frame.size);
		if (frame.size == 6 && frame.bytes[3] == 3 && frame.bytes[4] == 6) {
			k_sem_give(&advertising_done);
		}
		return frame.size;
	}
	if (!command || size < 4 || command[0] != 1 || command[3] != size - 4) {
		atomic_inc(&unexpected);
		return -EINVAL;
	}
	uint16_t opcode = sys_get_le16(command + 1);
	switch (opcode) {
	case 0xfc0c: {
		const uint8_t reply[] = {4, 0xff, 3, 0x0c, 4, 0};
		memcpy(event, reply, sizeof(reply));
		return sizeof(reply);
	}
	case 0x1009: {
		const uint8_t reply[] = {4, 0x0e, 10, 1, 9, 0x10, 0,
			0x10, 0x32, 0x54, 0x76, 0x98, 0xba};
		memcpy(event, reply, sizeof(reply));
		return sizeof(reply);
	}
	case 0x1001: {
		const uint8_t reply[] = {4, 0x0e, 12, 1, 1, 0x10, 0,
			8, 0x34, 0x12, 8, 0x0d, 0, 0, 0};
		memcpy(event, reply, sizeof(reply));
		return sizeof(reply);
	}
	case 0x2007: {
		const uint8_t reply[] = {4, 0x0e, 5, 1, 7, 0x20, 0, 0};
		memcpy(event, reply, sizeof(reply));
		return sizeof(reply);
	}
	case 0xfc01: vendor_done(0x0401, 0); return 0;
	case 0xfc21: vendor_done(0x0421, 0); return 0;
	case 0xfc06: vendor_done(0x0406, 0); return 0;
	case 0xfdfc:
		if (service >= ARRAY_SIZE(ranges) || attributes_left) break;
		service_start = ranges[service][0];
		attributes_left = ranges[service][1] - service_start;
		goto registered;
	case 0xfdfe:
		if (!attributes_left) break;
		attributes_left--;
		goto registered;
	registered:
		if (!attributes_left) {
			uint8_t reply[] = {4, 0xff, 10, 0x7f, 6, 0,
				(uint8_t)opcode, 0xfd, 4, 0, 0, 0, 0};
			sys_put_le16(service_start, reply + 9);
			sys_put_le16(ranges[service][1], reply + 11);
			service++;
			memcpy(event, reply, sizeof(reply));
			return sizeof(reply);
		}
		return command_status(opcode, 0, event);
	case 0xfe36:
	case 0xfe30:
	case 0xfe03:
		return command_status(opcode, 0, event);
	case 0xfe00:
		vendor_done(0x0600, 0);
		return command_status(opcode, 0, event);
	case 0xfe07:
		atomic_inc(&adv_updates);
		record(command, size);
		vendor_done(0x0602, 0);
		return command_status(opcode, 0, event);
	case 0xfe06:
		atomic_inc(&advertising_starts);
		record(command, size);
		vendor_done(0x0603, 0);
		return command_status(opcode, 0, event);
	case 0xfe0a: {
		record(command, size);
		if (size != 7) break;
		/* ACK first; the separate 0606 event closes this exact handle. */
		uint8_t terminated[] = {4, 0xff, 6, 6, 6, 0,
			command[4], command[5], command[6]};
		enqueue(terminated, sizeof(terminated));
		return command_status(opcode, 0, event);
	}
	case 0xfd1b:
		atomic_inc(&notifications);
		if (size > 9 && memchr(command + 9, '@', size - 9) &&
		    atomic_cas(&disconnect_on_marker, 1, 0)) {
			/* The link disappears while the worker waits for this command's
			 * status. A later ACK must not publish remaining old-session bytes. */
			uint8_t ack[9];
			command_status(opcode, 0, ack);
			enqueue(ack, sizeof(ack));
			const uint8_t terminated[] = {4, 0xff, 6, 6, 6, 0,
				command[4], command[5], 0x13};
			memcpy(event, terminated, sizeof(terminated));
			atomic_inc(&injected_disconnect);
			return sizeof(terminated);
		}
		if (atomic_cas(&reject_next_notification, 1, 0)) {
			first_notification_size = size;
			memcpy(first_notification, command, size);
			return command_status(opcode, 0x16, event);
		}
		if (first_notification_size && size == first_notification_size &&
		    !memcmp(command, first_notification, size)) atomic_inc(&retried);
		/* Only admitted notifications represent bytes delivered to the peer. */
		record(command, size);
		return command_status(opcode, 0, event);
	default:
		if ((opcode & 0xff00) == 0xfd00) {
			record(command, size);
			return command_status(opcode, 0, event);
		}
		break;
	}
	atomic_inc(&unexpected);
	return -ENOTSUP;
}

static void link_established(uint16_t handle)
{
	uint8_t event[] = {4, 0xff, 20, 5, 6, 0, 0,
		0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0, 0, 4,
		24, 0, 0, 0, 0xf4, 1, 0};
	sys_put_le16(handle, event + 13);
	enqueue(event, sizeof(event));
}

static void link_terminated(uint16_t handle)
{
	uint8_t event[] = {4, 0xff, 6, 6, 6, 0, 0, 0, 0x13};
	sys_put_le16(handle, event + 6);
	enqueue(event, sizeof(event));
}

static void att_write_flags(uint16_t connection, uint16_t handle,
			    const uint8_t *value, size_t length, bool command)
{
	uint8_t event[9 + 4 + 20] = {4, 0xff, 0, 0x12, 5, 0, 0, 0, 0};
	if (length > 20) {
		atomic_inc(&unexpected);
		return;
	}
	event[2] = 10 + length;
	sys_put_le16(connection, event + 6);
	event[8] = 4 + length;
	event[10] = command;
	sys_put_le16(handle, event + 11);
	memcpy(event + 13, value, length);
	enqueue(event, 13 + length);
}

static void att_write(uint16_t connection, uint16_t handle,
		      const uint8_t *value, size_t length)
{
	att_write_flags(connection, handle, value, length, false);
}

static void att_write_command(uint16_t connection, uint16_t handle,
			      const uint8_t *value, size_t length)
{
	att_write_flags(connection, handle, value, length, true);
}

static void discover_smp(uint16_t connection)
{
	uint8_t event[] = {4, 0xff, 12, 0x10, 5, 0, 0, 0, 6,
		34, 0, 37, 0, 0, 0x28};
	sys_put_le16(connection, event + 6);
	enqueue(event, sizeof(event));
}

static void set_smp_subscription(uint16_t connection, bool enabled)
{
	const uint8_t value[] = {enabled, 0};
	att_write(connection, 37, value, sizeof(value));
}

static void subscribe_shell(uint16_t connection)
{
	const uint8_t enabled[] = {1, 0};
	att_write(connection, 31, enabled, sizeof(enabled));
}

static void probe_shell(uint16_t connection)
{
	const uint8_t probe[] = {'p', 'r', 'o', 'b', 'e', '\r'};
	att_write(connection, 33, probe, sizeof(probe));
}

static int cmd_probe(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(shell, "CC2640 probe");
	k_sem_give(&command_done);
	return 0;
}
SHELL_CMD_REGISTER(probe, NULL, "Live CC2640 Shell probe", cmd_probe);

static int cmd_block(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(shell);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	k_sem_give(&block_entered);
	return k_sem_take(&block_release, K_SECONDS(1));
}
SHELL_CMD_REGISTER(block, NULL, "Hold an accepted Shell operation", cmd_block);

static int cmd_retire(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	/* Several notifications ensure both in-flight and queued old bytes exist. */
	shell_print(shell, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
	return 0;
}
SHELL_CMD_REGISTER(retire, NULL, "Exercise disconnect during output", cmd_retire);

static int smp_probe(struct smp_streamer *stream)
{
	ARG_UNUSED(stream);
	k_sem_give(&smp_done);
	return 0;
}

static int smp_block(struct smp_streamer *stream)
{
	ARG_UNUSED(stream);
	atomic_inc(&smp_block_calls);
	k_sem_give(&smp_block_entered);
	return k_sem_take(&smp_block_release, K_SECONDS(1)) ? MGMT_ERR_ETIMEOUT : 0;
}

/* SMP v1 OS echo, CBOR {"d": "abcdefghijklmnopqrstuvwx"}. Both request
 * and response exceed one default-MTU ATT value. */
static const uint8_t ble_echo_request[] = {
	0x0a, 0, 0, 29, 0, 0, 7, 0,
	0xa1, 0x61, 'd', 0x78, 24,
	'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
	'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
};
static const uint8_t ble_echo_response[] = {
	0x0b, 0, 0, 30, 0, 0, 7, 0,
	0xbf, 0x61, 'r', 0x78, 24,
	'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
	'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
	0xff,
};

static void send_ble_smp(uint16_t connection, const uint8_t *packet, size_t size)
{
	for (size_t sent = 0; sent < size;) {
		size_t count = MIN(size - sent, 20);
		att_write_command(connection, 36, packet + sent, count);
		sent += count;
	}
}

static uint16_t smp_connection;
static int send_smp(const void *data, int length)
{
	const uint8_t *bytes = data;
	for (int sent = 0; sent < length;) {
		int part = MIN(length - sent, 20);
		att_write(smp_connection, 33, bytes + sent, part);
		sent += part;
	}
	return 0;
}

static void await_status(enum host_ble_status status)
{
	int64_t deadline = k_uptime_get() + 1000;
	while (host_ble_status() != status && k_uptime_get() < deadline) k_sleep(K_MSEC(1));
	zassert_equal(host_ble_status(), status);
}

static void await_released(void)
{
	struct hub_power_state power;
	int64_t deadline = k_uptime_get() + 1000;
	do {
		hub_power_get(&power);
		if (!power.connected) return;
		k_sleep(K_MSEC(1));
	} while (k_uptime_get() < deadline);
	zassert_unreachable("Management power hold was not released");
}

static void check_initial_advertising(void)
{
	struct frame frame;
	zassert_ok(k_msgq_get(&outgoing, &frame, K_NO_WAIT));
	const uint8_t advertising_prefix[] = {1, 7, 0xfe, 23, 1, 21,
		2, 1, 6, 17, 6};
	zassert_true(frame.size >= sizeof(advertising_prefix));
	zassert_mem_equal(frame.bytes, advertising_prefix, sizeof(advertising_prefix));
	const uint8_t service_uuid[] = {0xef, 0xae, 0xe4, 0x51, 0x80, 0x6d,
		0xf4, 0x89, 0xda, 0x46, 0x80, 0x82, 1, 0, 0xf5, 0xc5};
	zassert_mem_equal(frame.bytes + 11, service_uuid, sizeof(service_uuid));
	zassert_ok(k_msgq_get(&outgoing, &frame, K_NO_WAIT));
	const uint8_t scan_response[] = {1, 7, 0xfe, 31, 0, 29, 17, 6,
		0x84, 0xaa, 0x60, 0x74, 0x52, 0x8a, 0x8b, 0x86,
		0xd3, 0x4c, 0xb7, 0x1d, 0x1d, 0xdc, 0x53, 0x8d,
		10, 9, 'B', 'r', 'i', 'c', 'k', 'p', 'h', 'y', 'r'};
	zassert_equal(frame.size, sizeof(scan_response));
	zassert_mem_equal(frame.bytes, scan_response, sizeof(scan_response));
	zassert_ok(k_msgq_get(&outgoing, &frame, K_NO_WAIT));
	const uint8_t discoverable[] = {1, 6, 0xfe, 10, 0, 2,
		0, 0, 0, 0, 0, 0, 7, 0};
	zassert_equal(frame.size, sizeof(discoverable));
	zassert_mem_equal(frame.bytes, discoverable, sizeof(discoverable));
}

static void expect_outgoing(const uint8_t *expected, size_t length)
{
	struct frame frame;
	zassert_ok(k_msgq_get(&outgoing, &frame, K_SECONDS(1)));
	zassert_equal(frame.size, length);
	zassert_mem_equal(frame.bytes, expected, length);
}

static void expect_smp_echo(uint16_t connection)
{
	for (size_t offset = 0; offset < sizeof(ble_echo_response);) {
		struct frame frame;
		zassert_ok(k_msgq_get(&outgoing, &frame, K_SECONDS(1)));
		size_t count = MIN(sizeof(ble_echo_response) - offset, 20);
		const uint8_t prefix[] = {1, 0x1b, 0xfd, count + 5,
			(uint8_t)connection, (uint8_t)(connection >> 8), 0, 36, 0};
		zassert_equal(frame.size, sizeof(prefix) + count);
		zassert_mem_equal(frame.bytes, prefix, sizeof(prefix));
		zassert_mem_equal(frame.bytes + sizeof(prefix), ble_echo_response + offset, count);
		offset += count;
	}
}

static void expect_smp_discovery(uint16_t connection)
{
	discover_smp(connection);
	const uint8_t response[] = {1, 0x11, 0xfd, 23,
		(uint8_t)connection, (uint8_t)(connection >> 8),
		20, 34, 0, 37, 0,
		0x84, 0xaa, 0x60, 0x74, 0x52, 0x8a, 0x8b, 0x86,
		0xd3, 0x4c, 0xb7, 0x1d, 0x1d, 0xdc, 0x53, 0x8d};
	expect_outgoing(response, sizeof(response));
	uint8_t characteristic[] = {4, 0xff, 12, 8, 5, 0, 0, 0, 6,
		34, 0, 37, 0, 3, 0x28};
	sys_put_le16(connection, characteristic + 6);
	enqueue(characteristic, sizeof(characteristic));
	const uint8_t declaration[] = {1, 9, 0xfd, 24,
		(uint8_t)connection, (uint8_t)(connection >> 8),
		21, 35, 0, 0x14, 36, 0,
		0x48, 0x7c, 0x99, 0x74, 0x11, 0x26, 0x9e, 0xae,
		0x01, 0x4e, 0xce, 0xfb, 0x28, 0x78, 0x2e, 0xda};
	expect_outgoing(declaration, sizeof(declaration));
}

static void expect_smp_subscription(uint16_t connection, bool enabled)
{
	set_smp_subscription(connection, enabled);
	const uint8_t response[] = {1, 0x13, 0xfd, 2,
		(uint8_t)connection, (uint8_t)(connection >> 8)};
	expect_outgoing(response, sizeof(response));
}

static void await_notification_text(const char *needle)
{
	char text[512] = {0};
	size_t used = 0;
	int64_t deadline = k_uptime_get() + 1000;
	while (k_uptime_get() < deadline && used < sizeof(text) - 1) {
		struct frame frame;
		if (k_msgq_get(&outgoing, &frame, K_MSEC(1))) continue;
		if (frame.size < 9 || frame.bytes[1] != 0x1b || frame.bytes[2] != 0xfd) continue;
		zassert_is_null(memchr(frame.bytes + 9, '@', frame.size - 9),
			"Old-session bytes crossed a connection boundary");
		size_t part = MIN(frame.size - 9, sizeof(text) - 1 - used);
		memcpy(text + used, frame.bytes + 9, part);
		used += part;
		if (strstr(text, needle)) return;
	}
	zassert_unreachable("Missing notification text: %s", needle);
}

ZTEST(cc2640_host, test_cold_shell_smp_backpressure_and_reconnect)
{
	zassert_ok(host_cc2640_init(RADIO, address));
	zassert_ok(k_sem_take(&advertising_done, K_SECONDS(1)));
	zassert_equal(atomic_get(&adv_updates), 2);
	zassert_equal(atomic_get(&advertising_starts), 1);
	check_initial_advertising();
	zassert_equal(atomic_get(&unexpected), 0);
	link_established(1);
	await_status(HOST_BLE_CONNECTED);
	subscribe_shell(1);
	await_status(HOST_BLE_NATIVE);
	atomic_set(&reject_next_notification, 1);
	probe_shell(1);
	zassert_ok(k_sem_take(&command_done, K_SECONDS(1)));
	await_notification_text("CC2640 probe");
	zassert_true(atomic_get(&retried) >= 1);

	static const struct mgmt_handler handlers[] = {
		{.mh_write = smp_probe}, {.mh_write = smp_block},
	};
	static struct mgmt_group group = {
		.mg_handlers = handlers, .mg_handlers_count = ARRAY_SIZE(handlers),
		.mg_group_id = MGMT_GROUP_ID_PERUSER,
	};
	mgmt_register_group(&group);
	smp_connection = 1;
	const uint8_t request[] = {0x0a, 0, 0, 1, 0, MGMT_GROUP_ID_PERUSER, 1, 0, 0xa0};
	zassert_ok(mcumgr_serial_tx_pkt(request, sizeof(request), send_smp));
	zassert_ok(k_sem_take(&smp_done, K_SECONDS(1)));

	/* Reconnection must wait for accepted Shell work, not merely link refs. */
	const uint8_t block[] = "block\r";
	att_write(1, 33, block, sizeof(block) - 1);
	zassert_ok(k_sem_take(&block_entered, K_SECONDS(1)));
	link_terminated(1);
	await_status(HOST_BLE_WAITING);
	zassert_equal(k_sem_take(&advertising_done, K_MSEC(20)), -EAGAIN,
		"Advertised before the disconnected Shell command retired");
	k_sem_give(&block_release);
	zassert_ok(k_sem_take(&advertising_done, K_SECONDS(1)));
	await_released();
	link_established(2);
	await_status(HOST_BLE_CONNECTED);
	subscribe_shell(2);
	await_status(HOST_BLE_NATIVE);
	probe_shell(2);
	zassert_ok(k_sem_take(&command_done, K_SECONDS(1)));
	await_notification_text("CC2640 probe");

	atomic_set(&disconnect_on_marker, 1);
	const uint8_t retire[] = "retire\r";
	att_write(2, 33, retire, sizeof(retire) - 1);
	zassert_ok(k_sem_take(&advertising_done, K_SECONDS(1)));
	zassert_equal(atomic_get(&injected_disconnect), 1);
	await_released();
	k_msgq_purge(&outgoing);
	/* Reusing the numeric handle must not reuse a still-retained session. */
	link_established(2);
	await_status(HOST_BLE_CONNECTED);
	subscribe_shell(2);
	await_status(HOST_BLE_NATIVE);
	probe_shell(2);
	zassert_ok(k_sem_take(&command_done, K_SECONDS(1)));
	await_notification_text("CC2640 probe");

	/* A fresh link needs only the standard SMP CCC. The earlier Shell
	 * subscription must not be a prerequisite for management requests. */
	link_terminated(2);
	await_status(HOST_BLE_WAITING);
	zassert_ok(k_sem_take(&advertising_done, K_SECONDS(1)));
	await_released();
	k_msgq_purge(&outgoing);
	link_established(2);
	await_status(HOST_BLE_CONNECTED);
	expect_smp_discovery(2);
	expect_smp_subscription(2, true);
	/* An unknown nested value must not bypass the bounded CBOR decoder.
	 * Valid echo data remains present, so accepting it would return an echo. */
	const uint8_t nested[] = {0x0a, 0, 0, 13, 0, 0, 8, 0,
		0xa2, 0x61, 'd', 0x61, 'x', 0x61, 'u', 0x81, 0x81, 0x81, 0x81, 0x81, 0x00};
	send_ble_smp(2, nested, sizeof(nested));
	const uint8_t invalid_reply[] = {1, 0x1b, 0xfd, 19, 2, 0, 0, 36, 0,
		0x0b, 0, 0, 6, 0, 0, 8, 0, 0xbf, 0x62, 'r', 'c', 3, 0xff};
	expect_outgoing(invalid_reply, sizeof(invalid_reply));
	send_ble_smp(2, ble_echo_request, sizeof(ble_echo_request));
	expect_smp_echo(2);
	await_status(HOST_BLE_NATIVE);

	/* The same numeric link can unsubscribe and subscribe again. Even an
	 * accepted command from the previous CCC generation cannot publish. */
	const uint8_t held[] = {0x0a, 0, 0, 1, 0, MGMT_GROUP_ID_PERUSER, 9, 1, 0xa0};
	const uint8_t queued[] = {0x0a, 0, 0, 1, 0, MGMT_GROUP_ID_PERUSER, 10, 1, 0xa0};
	send_ble_smp(2, held, sizeof(held));
	zassert_ok(k_sem_take(&smp_block_entered, K_SECONDS(1)));
	send_ble_smp(2, queued, sizeof(queued));
	att_write_command(2, 36, ble_echo_request, 20);
	expect_smp_subscription(2, false);
	expect_smp_subscription(2, true);
	send_ble_smp(2, ble_echo_request, sizeof(ble_echo_request));
	k_sem_give(&smp_block_release);
	expect_smp_echo(2);
	zassert_equal(atomic_get(&smp_block_calls), 1);

	/* Hold an accepted SMP command, then queue another complete command and
	 * leave an echo fragment incomplete. Disconnect must retire all three
	 * before a reused numeric handle can accept a new connection. */
	send_ble_smp(2, held, sizeof(held));
	zassert_ok(k_sem_take(&smp_block_entered, K_SECONDS(1)));
	send_ble_smp(2, queued, sizeof(queued));
	att_write_command(2, 36, ble_echo_request, 20);
	link_terminated(2);
	await_status(HOST_BLE_WAITING);
	zassert_equal(k_sem_take(&advertising_done, K_MSEC(20)), -EAGAIN,
		"Advertised while an SMP handler still held the old session");
	k_sem_give(&smp_block_release);
	zassert_ok(k_sem_take(&advertising_done, K_SECONDS(1)));
	await_released();
	zassert_equal(atomic_get(&smp_block_calls), 2,
		"Queued old-session SMP request reached its handler");
	struct frame frame;
	while (!k_msgq_get(&outgoing, &frame, K_NO_WAIT)) {
		zassert_false(frame.size >= 9 && frame.bytes[1] == 0x1b &&
			frame.bytes[2] == 0xfd && sys_get_le16(frame.bytes + 7) == 36,
			"Old-session SMP notification escaped after disconnect");
	}
	link_established(2);
	await_status(HOST_BLE_CONNECTED);
	expect_smp_subscription(2, true);
	send_ble_smp(2, ble_echo_request, sizeof(ble_echo_request));
	expect_smp_echo(2);
	zassert_equal(atomic_get(&smp_block_calls), 2);
	mgmt_unregister_group(&group);
	host_ble_shutdown();
	await_status(HOST_BLE_OFF);
	await_released();
	zassert_equal(atomic_get(&unexpected), 0);
}

ZTEST_SUITE(cc2640_host, NULL, NULL, NULL, NULL, NULL);
