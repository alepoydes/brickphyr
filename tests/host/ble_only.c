// SPDX-License-Identifier: Apache-2.0
#include "host_ble_session.h"
#include "host_protocol.h"

#include <errno.h>
#include <string.h>
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
	uint8_t caps[HOST_PROTOCOL_PAYLOAD_MAX];
	zassert_true(host_ble_session_read(peer, true, 3, caps, sizeof(caps), 0) > 0);
	zassert_equal(host_ble_session_profile(), HOST_BLE_PYBRICKS);
	zassert_equal(host_ble_session_shell_subscribe(peer, 1), 2);
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
	int result = host_ble_session_shell_input(peer, data, length, 0, false);
	return result < 0 ? result : 0;
}

ZTEST(ble_only, test_cold_shell_smp_and_reconnect)
{
	zassert_ok(host_transport_init(NULL, 0));
	/* No USB device, UART IRQ or status query may be needed to start Shell. */
	connect_shell();
	zassert_equal(host_interface_select(HOST_PYBRICKS), -ENOTSUP);
	host_connection_changed(true);
	zassert_equal(host_ble_session_shell_input(peer, "probe\r", 6, 0, false), 6);
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
	peer = &links[1];
	connect_shell();
	zassert_equal(host_ble_session_shell_input(&links[0], "probe\r", 6, 0, false),
		      -BT_ATT_ERR_AUTHORIZATION);
	zassert_equal(host_ble_session_shell_input(peer, "probe\r", 6, 0, false), 6);
	zassert_ok(k_sem_take(&command_done, K_SECONDS(1)));
	receive_until("BLE-only probe");
	zassert_equal(calls, 3);
	host_ble_session_close(peer);
	zassert_equal(atomic_get(&links[0].references), 0);
	zassert_equal(atomic_get(&links[1].references), 0);
	mgmt_unregister_group(&group);
}

ZTEST_SUITE(ble_only, NULL, NULL, NULL, NULL, NULL);
