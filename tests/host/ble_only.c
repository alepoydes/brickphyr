// SPDX-License-Identifier: Apache-2.0
#include "host_transport.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/transport/serial.h>
#include <zephyr/shell/shell.h>
#include <zephyr/ztest.h>

static uint32_t session;
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

static void output_ready(void) {}

static void connect_shell(void)
{
	int err;
	int64_t deadline = k_uptime_get() + 1000;
	while ((err = host_ble_acquire()) == -EBUSY && k_uptime_get() < deadline) {
		k_sleep(K_MSEC(1));
	}
	zassert_ok(err);
	zassert_ok(host_ble_shell_open(output_ready, &session));
}

static void receive_until(const char *ending)
{
	char output[512] = {0};
	size_t length = 0;
	int64_t deadline = k_uptime_get() + 1000;
	while (k_uptime_get() < deadline && length < sizeof(output) - 1) {
		size_t part = host_ble_shell_peek(session, output + length,
						sizeof(output) - 1 - length);
		host_ble_shell_consume(session, part);
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
	return host_ble_shell_receive(session, data, length);
}

ZTEST(ble_only, test_cold_shell_smp_and_reconnect)
{
	zassert_ok(host_transport_init(NULL, 0));
	/* No USB device, UART IRQ or status query may be needed to start Shell. */
	connect_shell();
	zassert_equal(host_interface_select(HOST_PYBRICKS), -ENOTSUP);
	host_connection_changed(true);
	zassert_ok(host_ble_shell_receive(session, "probe\r", 6));
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
	uint32_t old_session = session;
	host_ble_release(false);
	connect_shell();
	zassert_equal(host_ble_shell_receive(old_session, "probe\r", 6), -ENOTCONN);
	zassert_ok(host_ble_shell_receive(session, "probe\r", 6));
	zassert_ok(k_sem_take(&command_done, K_SECONDS(1)));
	receive_until("BLE-only probe");
	zassert_equal(calls, 3);
	host_ble_release(false);
	mgmt_unregister_group(&group);
}

ZTEST_SUITE(ble_only, NULL, NULL, NULL, NULL, NULL);
