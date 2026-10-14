// SPDX-License-Identifier: Apache-2.0
#include "host_ble_session.h"
#include "host_protocol.h"
#include "hub_power.h"
#ifdef CC2640_GATT
#include "cc2640_gatt.h"
#endif

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/transport/serial.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/ztest.h>

struct host_ble_link { atomic_t references; };
static struct host_ble_link links[2];
static struct host_ble_link *peer = &links[0];
static struct k_spinlock output_lock;
RING_BUF_DECLARE(output_bytes, 256);
static unsigned int calls;
static K_SEM_DEFINE(command_done, 0, 1);

#ifdef CC2640_GATT
static struct cc2640_gatt gatt;
static bool first_gatt_link = true;

static void gatt_expect(const uint8_t *event, size_t event_size,
			const uint8_t *expected, size_t expected_size)
{
	uint8_t packet[CC2640_GATT_PACKET_MAX] = {0};
	int size = cc2640_gatt_request(&gatt, peer, 1, event, event_size, packet);
	zassert_equal(size, expected_size);
	zassert_mem_equal(packet, expected, expected_size);
}

#define GATT_EXPECT(event, expected) \
	gatt_expect(event, sizeof(event), expected, sizeof(expected))

static void gatt_register(void)
{
	uint8_t packet[CC2640_GATT_PACKET_MAX] = {0};
	/* GATT AddService: primary UUID 0x2800, one attribute, key size seven. */
	const uint8_t primary[] = {0x01, 0xfc, 0xfd, 0x05, 0x00, 0x28, 0x01, 0x00, 0x07};
	zassert_equal(cc2640_gatt_registration(0, 0, packet), sizeof(primary));
	zassert_mem_equal(packet, primary, sizeof(primary));
	/* 128-bit Pybricks command/event UUID is sent in Bluetooth wire order. */
	const uint8_t command[] = {
		0x01, 0xfe, 0xfd, 0x11, 0xef, 0xae, 0xe4, 0x51, 0x80, 0x6d,
		0xf4, 0x89, 0xda, 0x46, 0x80, 0x82, 0x02, 0x00, 0xf5, 0xc5, 0x03,
	};
	zassert_equal(cc2640_gatt_registration(3, 2, packet), sizeof(command));
	zassert_mem_equal(packet, command, sizeof(command));
	zassert_equal(cc2640_gatt_registration(6, 0, packet), 0);
	const uint16_t first[] = {1, 2, 9, 16, 22, 28};
	const uint16_t last[] = {1, 8, 15, 21, 27, 33};
	for (size_t service = 0; service < ARRAY_SIZE(first); service++) {
		zassert_ok(cc2640_gatt_bind(&gatt, service, first[service], last[service]));
	}
	zassert_equal(cc2640_gatt_bind(&gatt, 3, 16, 20), -EBADMSG);
	zassert_equal(gatt.first[3], 16);
}

static void gatt_discover_and_reject(void)
{
	/* Read By Group Type 0x2800 discovers the complete 128-bit service range. */
	const uint8_t group[] = {0x04, 0xff, 0x0c, 0x10, 0x05, 0, 1, 0,
		0x06, 0x10, 0, 0x21, 0, 0, 0x28};
	const uint8_t group_reply[] = {0x01, 0x11, 0xfd, 0x17, 1, 0, 0x14,
		0x10, 0, 0x15, 0, 0xef, 0xae, 0xe4, 0x51, 0x80, 0x6d, 0xf4,
		0x89, 0xda, 0x46, 0x80, 0x82, 0x01, 0, 0xf5, 0xc5};
	GATT_EXPECT(group, group_reply);
	/* Find By Type Value returns the same end handle for that service UUID. */
	const uint8_t find[] = {0x04, 0xff, 0x1c, 0x06, 0x05, 0, 1, 0,
		0x16, 0x10, 0, 0x21, 0, 0, 0x28, 0xef, 0xae, 0xe4, 0x51,
		0x80, 0x6d, 0xf4, 0x89, 0xda, 0x46, 0x80, 0x82, 0x01, 0, 0xf5, 0xc5};
	const uint8_t find_reply[] = {0x01, 0x07, 0xfd, 0x06, 1, 0, 0x10, 0, 0x15, 0};
	GATT_EXPECT(find, find_reply);
	/* Read By Type 0x2803 discovers characteristic properties and 128-bit UUID. */
	const uint8_t characteristic[] = {0x04, 0xff, 0x0c, 0x08, 0x05, 0, 1, 0,
		0x06, 0x11, 0, 0x15, 0, 0x03, 0x28};
	const uint8_t characteristic_reply[] = {0x01, 0x09, 0xfd, 0x18, 1, 0,
		0x15, 0x11, 0, 0x18, 0x12, 0, 0xef, 0xae, 0xe4, 0x51, 0x80,
		0x6d, 0xf4, 0x89, 0xda, 0x46, 0x80, 0x82, 0x02, 0, 0xf5, 0xc5};
	GATT_EXPECT(characteristic, characteristic_reply);
	/* An out-of-range discovery has an ATT error, not a fabricated attribute. */
	const uint8_t missing[] = {0x04, 0xff, 0x0c, 0x08, 0x05, 0, 1, 0,
		0x06, 0x22, 0, 0x24, 0, 0x03, 0x28};
	const uint8_t missing_reply[] = {0x01, 0x01, 0xfd, 0x06, 1, 0,
		0x08, 0x22, 0, BT_ATT_ERR_ATTRIBUTE_NOT_FOUND};
	GATT_EXPECT(missing, missing_reply);
	const uint8_t read_notify_only[] = {0x04, 0xff, 0x08, 0x0a, 0x05, 0, 1, 0,
		0x02, 0x1e, 0};
	const uint8_t read_error[] = {0x01, 0x01, 0xfd, 0x06, 1, 0,
		0x0a, 0x1e, 0, BT_ATT_ERR_READ_NOT_PERMITTED};
	GATT_EXPECT(read_notify_only, read_error);
	const uint8_t write_service[] = {0x04, 0xff, 0x0a, 0x12, 0x05, 0, 1, 0,
		0x04, 0, 0, 0x10, 0};
	const uint8_t write_error[] = {0x01, 0x01, 0xfd, 0x06, 1, 0,
		0x12, 0x10, 0, BT_ATT_ERR_WRITE_NOT_PERMITTED};
	GATT_EXPECT(write_service, write_error);
	const uint8_t bad_offset[] = {0x04, 0xff, 0x0a, 0x0c, 0x05, 0, 1, 0,
		0x04, 0x1f, 0, 3, 0};
	const uint8_t offset_error[] = {0x01, 0x01, 0xfd, 0x06, 1, 0,
		0x0c, 0x1f, 0, BT_ATT_ERR_INVALID_OFFSET};
	GATT_EXPECT(bad_offset, offset_error);
	zassert_equal(calls, 0);
}

static void gatt_subscribe_shell(void)
{
	const uint8_t read_ccc[] = {0x04, 0xff, 0x08, 0x0a, 0x05, 0, 1, 0,
		0x02, 0x1f, 0};
	const uint8_t empty_ccc[] = {0x01, 0x0b, 0xfd, 0x04, 1, 0, 0, 0};
	GATT_EXPECT(read_ccc, empty_ccc);
	const uint8_t malformed[] = {0x04, 0xff, 0x0b, 0x12, 0x05, 0, 1, 0,
		0x05, 0, 0, 0x1f, 0, 1};
	const uint8_t length_error[] = {0x01, 0x01, 0xfd, 0x06, 1, 0,
		0x12, 0x1f, 0, BT_ATT_ERR_INVALID_ATTRIBUTE_LEN};
	GATT_EXPECT(malformed, length_error);
	const uint8_t invalid_value[] = {0x04, 0xff, 0x0c, 0x12, 0x05, 0, 1, 0,
		0x06, 0, 0, 0x1f, 0, 2, 0};
	const uint8_t value_error[] = {0x01, 0x01, 0xfd, 0x06, 1, 0,
		0x12, 0x1f, 0, BT_ATT_ERR_VALUE_NOT_ALLOWED};
	GATT_EXPECT(invalid_value, value_error);
	GATT_EXPECT(read_ccc, empty_ccc);
	zassert_equal(host_ble_session_profile(), HOST_BLE_PYBRICKS);
	const uint8_t enable[] = {0x04, 0xff, 0x0c, 0x12, 0x05, 0, 1, 0,
		0x06, 0, 0, 0x1f, 0, 1, 0};
	const uint8_t write_reply[] = {0x01, 0x13, 0xfd, 0x02, 1, 0};
	GATT_EXPECT(enable, write_reply);
	const uint8_t enabled_ccc[] = {0x01, 0x0b, 0xfd, 0x04, 1, 0, 1, 0};
	GATT_EXPECT(read_ccc, enabled_ccc);
}

static int gatt_shell_write(const uint8_t *data, size_t length)
{
	uint8_t event[9 + 4 + 20] = {0x04, 0xff, 0, 0x12, 0x05, 0, 1, 0, 0};
	uint8_t packet[CC2640_GATT_PACKET_MAX] = {0};
	event[11] = 0x21; /* Native Shell RX handle 33. */
	const uint8_t expected[] = {0x01, 0x13, 0xfd, 0x02, 1, 0};
	for (size_t sent = 0; sent < length;) {
		size_t part = MIN(length - sent, 20);
		event[2] = 10 + part;
		event[8] = 4 + part;
		memcpy(event + 13, data + sent, part);
		int size = cc2640_gatt_request(&gatt, peer, 1, event, 13 + part, packet);
		if (size < 0) return size;
		zassert_equal(size, sizeof(expected));
		zassert_mem_equal(packet, expected, sizeof(expected));
		sent += part;
	}
	return 0;
}
#endif

static int cmd_probe(const struct shell *shell, size_t argc, char **argv)
{
	calls++;
	shell_print(shell, "BLE-only probe");
	k_sem_give(&command_done);
	return 0;
}
SHELL_CMD_REGISTER(probe, NULL, "Exercise the production Shell.", cmd_probe);

struct host_ble_link *host_ble_link_ref(struct host_ble_link *link)
{
	atomic_inc(&link->references);
	return link;
}

void host_ble_link_unref(struct host_ble_link *link)
{
	zassert_true(atomic_dec(&link->references) > 0);
}

uint16_t host_ble_link_mtu(struct host_ble_link *link) { return 23; }

int host_ble_link_notify(struct host_ble_link *link, enum host_ble_value value,
			const uint8_t *data, size_t size)
{
	zassert_equal(value, HOST_BLE_VALUE_SHELL);
	zassert_true(size <= 20);
	k_spinlock_key_t key = k_spin_lock(&output_lock);
	int err = link != peer ? -ENOTCONN :
		ring_buf_space_get(&output_bytes) < size ? -EAGAIN : 0;
	if (!err) ring_buf_put(&output_bytes, data, size);
	k_spin_unlock(&output_lock, key);
	return err;
}

void host_ble_link_disconnect(struct host_ble_link *link)
{
	zassert_unreachable("Unexpected terminal failure");
}

static void connect_shell(void)
{
	int err;
	int64_t deadline = k_uptime_get() + 1000;
	while ((err = host_ble_session_open(peer)) == -EBUSY && k_uptime_get() < deadline) {
		k_sleep(K_MSEC(1));
	}
	zassert_ok(err);
#ifdef CC2640_GATT
	/* Reject a bad capabilities offset before it can select a profile. */
	const uint8_t bad_caps_offset[] = {4, 0xff, 10, 0x0c, 5, 0, 1, 0, 4, 21, 0, 12, 0};
	const uint8_t bad_caps_reply[] = {1, 1, 0xfd, 6, 1, 0, 0x0c, 21, 0, 7};
	GATT_EXPECT(bad_caps_offset, bad_caps_reply);
	zassert_equal(host_ble_session_profile(), HOST_BLE_CONNECTED);
	const uint8_t read_caps[] = {4, 0xff, 8, 0x0a, 5, 0, 1, 0, 2, 21, 0};
	const uint8_t caps_reply[] = {1, 0x0b, 0xfd, 13, 1, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	GATT_EXPECT(read_caps, caps_reply);
#else
	uint8_t caps[HOST_PROTOCOL_PAYLOAD_MAX];
	zassert_true(host_ble_session_read(peer, true, 3, caps, sizeof(caps), 0) > 0);
#endif
	zassert_equal(host_ble_session_profile(), HOST_BLE_PYBRICKS);
#ifdef CC2640_GATT
	if (first_gatt_link) {
		gatt_discover_and_reject();
		first_gatt_link = false;
	}
	gatt_subscribe_shell();
#else
	zassert_equal(host_ble_session_shell_subscribe(peer, 1), 2);
#endif
	zassert_equal(host_ble_session_profile(), HOST_BLE_NATIVE);
}

static void receive_until(const char *ending)
{
	char output[512] = {0};
	size_t length = 0;
	int64_t deadline = k_uptime_get() + 1000;
	while (k_uptime_get() < deadline && length < sizeof(output) - 1) {
		k_spinlock_key_t key = k_spin_lock(&output_lock);
		size_t part = ring_buf_get(&output_bytes, output + length,
					  sizeof(output) - 1 - length);
		k_spin_unlock(&output_lock, key);
		length += part;
		if (strstr(output, ending)) return;
		k_sleep(K_MSEC(1));
	}
	zassert_unreachable("Missing %s in %s", ending, output);
}

static int smp_probe(struct smp_streamer *stream)
{
	calls++;
	k_sem_give(&command_done);
	return 0;
}

static int send_smp(const void *data, int length)
{
#ifdef CC2640_GATT
	return gatt_shell_write(data, length);
#else
	int result = host_ble_session_shell_input(peer, data, length, 0, false);
	return result < 0 ? result : 0;
#endif
}

ZTEST(ble_only, test_cold_shell_smp_and_reconnect)
{
	const struct device *peripheral_uart = DEVICE_DT_GET(DT_NODELABEL(test_uart));
	struct hub_power_state power;
	zassert_true(device_is_ready(peripheral_uart));
	zassert_equal(host_transport_init(peripheral_uart, 0), -ENOTSUP);
	host_connection_changed(true);
	hub_power_get(&power);
	zassert_equal(power.transport, HUB_DISCONNECTED);
	zassert_false(power.connected);
	zassert_ok(host_transport_init(NULL, 0));
	/* A UART peripheral does not create a USB management endpoint. */
#ifdef CC2640_GATT
	gatt_register();
#endif
	connect_shell();
	zassert_equal(host_interface_select(HOST_PYBRICKS), -ENOTSUP);
	host_connection_changed(true);
#ifdef CC2640_GATT
	const uint8_t probe_event[] = {0x04, 0xff, 0x10, 0x12, 0x05, 0, 1, 0,
		0x0a, 0, 0, 0x21, 0, 'p', 'r', 'o', 'b', 'e', '\r'};
	const uint8_t write_reply[] = {0x01, 0x13, 0xfd, 0x02, 1, 0};
	GATT_EXPECT(probe_event, write_reply);
#else
	zassert_equal(host_ble_session_shell_input(peer, "probe\r", 6, 0, false), 6);
#endif
	zassert_ok(k_sem_take(&command_done, K_SECONDS(1)));
	receive_until("BLE-only probe");
	zassert_equal(calls, 1);

	/* SMP's real serial codec and dispatcher run on that same Shell stream. */
	static const struct mgmt_handler handlers[] = {{.mh_write = smp_probe}};
	static struct mgmt_group group = {
		.mg_handlers = handlers, .mg_handlers_count = ARRAY_SIZE(handlers),
		.mg_group_id = MGMT_GROUP_ID_PERUSER,
	};
	mgmt_register_group(&group);
	const uint8_t request[] = {0x0a, 0, 0, 1, 0, MGMT_GROUP_ID_PERUSER, 1, 0, 0xa0};
	zassert_ok(mcumgr_serial_tx_pkt(request, sizeof(request), send_smp));
	zassert_ok(k_sem_take(&command_done, K_SECONDS(1)));
	zassert_equal(calls, 2);
	host_ble_session_close(peer);
#ifdef CC2640_GATT
	/* The backend retains bound handles but starts each link with fresh CCCs. */
	memset(gatt.subscribed, 0, sizeof(gatt.subscribed));
#endif
	peer = &links[1];
	connect_shell();
	zassert_equal(host_ble_session_shell_input(&links[0], "probe\r", 6, 0, false),
		      -BT_ATT_ERR_AUTHORIZATION);
#ifdef CC2640_GATT
	zassert_ok(gatt_shell_write((const uint8_t *)"probe\r", 6));
#else
	zassert_equal(host_ble_session_shell_input(peer, "probe\r", 6, 0, false), 6);
#endif
	zassert_ok(k_sem_take(&command_done, K_SECONDS(1)));
	receive_until("BLE-only probe");
	zassert_equal(calls, 3);
	host_ble_session_close(peer);
	zassert_equal(atomic_get(&links[0].references), 0);
	zassert_equal(atomic_get(&links[1].references), 0);
	mgmt_unregister_group(&group);
}

ZTEST_SUITE(ble_only, NULL, NULL, NULL, NULL, NULL);
