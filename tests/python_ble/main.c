// SPDX-License-Identifier: Apache-2.0
#include "application.h"
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
#include "app_store.h"
#endif
#include "host_protocol.h"
#include "host_transport.h"
#include <pbio/protocol.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
#include <zcbor_decode.h>

/* Synthetic controller/GATT peer. The shared owner, BLE adapter, protocol,
 * protected VM and application supervisor are the production implementations.
 * An emulated UART bounds unread USB output; UART0 carries test results. */
static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(test_uart));
static uintptr_t peer_token;
static struct bt_conn *const peer = (void *)&peer_token;
static atomic_t refs, reject_notifications;
static atomic_t reject_app_notifications;
static bt_le_scan_cb_t *scan_callback;
static atomic_t scan_starts, scan_stops;
static atomic_t negotiated_mtu = ATOMIC_INIT(23);
static atomic_t shell_max_notify;
static bool peer_connected;
static unsigned int notifications_retried;
static unsigned int app_notifications_retried;
static struct k_work_q rx_queue;
static K_KERNEL_STACK_DEFINE(rx_stack, CONFIG_BT_RX_STACK_SIZE);
static K_SEM_DEFINE(command_done, 0, 1);
static uint8_t command_data[20];
static size_t command_size;
static ssize_t command_result;
static const struct bt_gatt_attr *command_attribute;
static struct k_thread supervisor;
static K_THREAD_STACK_DEFINE(supervisor_stack, 4096);
static atomic_t supervisor_stop;

/* Same periodic lifecycle service as the hub loop; status reads must not be
 * needed to reap a returned program before its stopped event can be sent. */
static void poll_applications(void *a, void *b, void *c)
{
	while (!atomic_get(&supervisor_stop)) {
		application_poll();
		k_msleep(20);
	}
}

struct event { uint16_t length; uint8_t data[61]; };
K_MSGQ_DEFINE(events, sizeof(struct event), 32, 4);
extern const struct bt_gatt_service_static pybricks_service;
extern const struct bt_gatt_service_static brickphyr_terminal;

int hub_bluetooth_power_off(void) { return 0; }
uint32_t hub_buttons(void) { return 0; }
int storage_status(void) { return -ENOTSUP; }
/* Execute the real service-pack loader against a synthetic CC2564C. This
 * checks the actual embedded metadata through BLERadio.version(), with no
 * controller I/O and without a separate copy of the parser in the fixture. */
int bt_h4_vnd_setup(const struct device *uart, const struct bt_hci_setup_params *params);
NET_BUF_POOL_FIXED_DEFINE(version_replies, 1,
	sizeof(struct bt_hci_rp_read_local_version_info), 0, NULL);
int __wrap_bt_hci_cmd_send_sync(uint16_t opcode, struct net_buf *command, struct net_buf **reply)
{
	if (opcode == BT_HCI_OP_READ_LOCAL_VERSION_INFO) {
		*reply = net_buf_alloc(&version_replies, K_NO_WAIT);
		zassert_not_null(*reply);
		struct bt_hci_rp_read_local_version_info version = {
			.manufacturer = sys_cpu_to_le16(0x000d),
			.lmp_subversion = sys_cpu_to_le16(0x9a1a),
		};
		net_buf_add_mem(*reply, &version, sizeof(version));
	}
	if (opcode == 0xfd0c) zassert_equal(command->data[1], 0, "eHCILL must stay disabled");
	if (command) net_buf_unref(command);
	return 0;
}
int __wrap_bt_enable(bt_ready_cb_t cb) { return bt_h4_vnd_setup(NULL, NULL); }
int __wrap_bt_disable(void) { return 0; }
int __wrap_bt_le_adv_start(const struct bt_le_adv_param *p, const struct bt_data *a,
	size_t n, const struct bt_data *s, size_t m) { return 0; }
int __wrap_bt_le_adv_stop(void) { return 0; }
int __wrap_bt_le_scan_start(const struct bt_le_scan_param *param, bt_le_scan_cb_t cb)
{
	zassert_not_equal(k_current_get(), &k_sys_work_q.thread);
	zassert_equal(param->type, BT_LE_SCAN_TYPE_PASSIVE);
	zassert_equal(param->options, 0); /* Repeated values must refresh age/RSSI. */
	zassert_equal(param->interval, 0x70);
	zassert_equal(param->window, 0x38);
	zassert_is_null(scan_callback);
	scan_callback = cb;
	atomic_inc(&scan_starts);
	return 0;
}
int __wrap_bt_le_scan_stop(void)
{
	zassert_not_equal(k_current_get(), &k_sys_work_q.thread);
	zassert_not_null(scan_callback);
	scan_callback = NULL;
	atomic_inc(&scan_stops);
	return 0;
}
struct bt_conn *__wrap_bt_conn_ref(struct bt_conn *conn)
{
	zassert_equal(conn, peer);
	atomic_inc(&refs);
	return conn;
}
void __wrap_bt_conn_unref(struct bt_conn *conn)
{
	zassert_equal(conn, peer);
	zassert_true(atomic_dec(&refs) > 0);
}
int __wrap_bt_conn_disconnect(struct bt_conn *conn, uint8_t reason)
{
	zassert_unreachable("Unexpected adapter disconnect");
	return -EIO;
}
int __wrap_bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info)
{
	zassert_equal(conn, peer);
	memset(info, 0, sizeof(*info));
	info->type = BT_CONN_TYPE_LE;
	info->role = BT_CONN_ROLE_PERIPHERAL;
	info->state = peer_connected ? BT_CONN_STATE_CONNECTED : BT_CONN_STATE_DISCONNECTED;
	return 0;
}
bool __wrap_bt_gatt_is_subscribed(struct bt_conn *conn,
	const struct bt_gatt_attr *attr, uint16_t ccc) { return true; }
uint16_t __wrap_bt_gatt_get_mtu(struct bt_conn *conn)
{
	zassert_equal(conn, peer);
	return atomic_get(&negotiated_mtu);
}
int __wrap_bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params)
{
	zassert_equal(conn, peer);
	zassert_true(params->len <= atomic_get(&negotiated_mtu) - 3);
	zassert_true(params->len <= sizeof(((struct event *)0)->data));
	bool app_message = params->attr == &pybricks_service.attrs[2] &&
		((const uint8_t *)params->data)[0] == PBIO_PYBRICKS_EVENT_WRITE_APP_DATA;
	if (atomic_get(&reject_notifications) || (app_message && atomic_get(&reject_app_notifications))) {
		if (atomic_get(&reject_notifications)) atomic_dec(&reject_notifications);
		notifications_retried++;
		if (app_message) app_notifications_retried++;
		return -ENOMEM;
	}
	if (params->attr == &brickphyr_terminal.attrs[2] &&
	    params->len > atomic_get(&shell_max_notify)) {
		atomic_set(&shell_max_notify, params->len);
	}
	struct event event = {.length = params->len};
	memcpy(event.data, params->data, params->len);
	int err = k_msgq_put(&events, &event, K_NO_WAIT) ? -ENOMEM : 0;
	if (!err && params->func) {
		params->func(conn, params->user_data);
	}
	return err;
}

static void receive_command(struct k_work *work)
{
	const struct bt_gatt_attr *attr = command_attribute;
	command_result = attr->write(peer, attr, command_data, command_size, 0, 0);
	k_sem_give(&command_done);
}
static K_WORK_DEFINE(command_work, receive_command);

static ssize_t gatt_write(const struct bt_gatt_attr *attr, const void *data, size_t length)
{
	zassert_true(length <= sizeof(command_data));
	command_attribute = attr;
	memcpy(command_data, data, length);
	command_size = length;
	k_work_submit_to_queue(&rx_queue, &command_work);
	zassert_ok(k_sem_take(&command_done, K_SECONDS(1)), "GATT command did not return");
	return command_result;
}

static ssize_t command(const void *data, size_t length)
{
	return gatt_write(&pybricks_service.attrs[2], data, length);
}

static void connected(bool open)
{
	peer_connected = open;
	STRUCT_SECTION_FOREACH(bt_conn_cb, cb) {
		if (open && cb->connected) { cb->connected(peer, 0); }
		if (!open && cb->disconnected) { cb->disconnected(peer, 0); }
	}
	if (!open) { return; }
	const struct bt_gatt_attr *ccc = &pybricks_service.attrs[3];
	struct bt_gatt_ccc_managed_user_data *cfg = ccc->user_data;
	zassert_equal(cfg->cfg_write(peer, ccc, BT_GATT_CCC_NOTIFY), 2);
	cfg->cfg_changed(ccc, BT_GATT_CCC_NOTIFY);
}

static ssize_t transfer(const uint8_t *module, size_t length)
{
	uint8_t packet[20] = {PBIO_PYBRICKS_COMMAND_WRITE_USER_PROGRAM_META};
	zassert_equal(command(packet, 5), 5);
	uint8_t header[13];
	sys_put_le32(length, header);
	memcpy(header + 4, "__main__", 9);
	const uint8_t *parts[] = {header, module};
	const size_t sizes[] = {sizeof(header), length};
	size_t offset = 0;
	for (size_t part = 0; part < ARRAY_SIZE(parts); part++) {
		for (size_t position = 0; position < sizes[part];) {
			size_t chunk = MIN(sizes[part] - position, sizeof(packet) - 5);
			packet[0] = PBIO_PYBRICKS_COMMAND_WRITE_USER_RAM;
			sys_put_le32(offset, packet + 1);
			memcpy(packet + 5, parts[part] + position, chunk);
			zassert_equal(command(packet, chunk + 5), chunk + 5);
			position += chunk;
			offset += chunk;
		}
	}
	packet[0] = PBIO_PYBRICKS_COMMAND_WRITE_USER_PROGRAM_META;
	sys_put_le32(offset, packet + 1);
	return command(packet, 5);
}

static void download(const uint8_t *module, size_t length)
{
	zassert_equal(transfer(module, length), 5);
}

static char output[4608];
static size_t output_size;
static bool running;
static uint8_t message_data[63];
static size_t message_size;
static void record_event(const uint8_t *data, size_t length, bool ble)
{
	if (data[0] == PBIO_PYBRICKS_EVENT_STATUS_REPORT) {
		uint32_t flags = sys_get_le32(data + 1);
		zassert_true(flags & BIT(ble ? PBIO_PYBRICKS_STATUS_BLE_HOST_CONNECTED :
			PBIO_PYBRICKS_STATUS_USB_HOST_CONNECTED));
		running = flags & BIT(PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING);
	} else if (data[0] == PBIO_PYBRICKS_EVENT_WRITE_APP_DATA) {
		message_size = length - 1;
		zassert_true(message_size <= sizeof(message_data));
		memcpy(message_data, data + 1, message_size);
	} else {
		zassert_equal(data[0], PBIO_PYBRICKS_EVENT_WRITE_STDOUT);
		zassert_true(output_size + length < sizeof(output));
		memcpy(output + output_size, data + 1, length - 1);
		output_size += length - 1;
		output[output_size] = 0;
	}
}

static void event(void)
{
	struct event event;
	int err = k_msgq_get(&events, &event, K_SECONDS(1));
	zassert_ok(err, "Missing event: running %d, output '%s'", running, output);
	record_event(event.data, event.length, true);
}

static void start(const char *ready)
{
	output_size = 0;
	output[0] = 0;
	const uint8_t start = PBIO_PYBRICKS_COMMAND_START_USER_PROGRAM;
	zassert_equal(command(&start, 1), 1);
	int64_t deadline = k_uptime_get() + 2000;
	do {
		event();
		zassert_true(k_uptime_get() < deadline, "%s", output);
	} while (!running || !strstr(output, ready));
}

static const uint8_t echo[] = {
#include <echo.inc>
};
static const uint8_t spin[] = {
#include <spin.inc>
};
static const uint8_t flood[] = {
#include <flood.inc>
};
static const uint8_t native[] = {
#include <native.inc>
};

static void *setup(void)
{
	k_thread_create(&supervisor, supervisor_stack, K_THREAD_STACK_SIZEOF(supervisor_stack),
		poll_applications, NULL, NULL, NULL, 0, 0, K_NO_WAIT);
	zassert_ok(host_transport_init(uart, 0));
	k_work_queue_start(&rx_queue, rx_stack, CONFIG_BT_RX_STACK_SIZE,
		K_PRIO_COOP(CONFIG_BT_RX_PRIO), NULL);
	uint8_t state[8];
	int64_t deadline = k_uptime_get() + 1000;
	do {
		k_msleep(1);
		host_interface_status(state);
		zassert_true(k_uptime_get() < deadline);
	} while (!state[3] || host_ble_status() != HOST_BLE_WAITING);
	return NULL;
}

ZTEST(python_ble, test_execution_reconnect_and_backpressure)
{
	notifications_retried = 0;
	atomic_set(&reject_notifications, 0);
	k_msgq_purge(&events);
	connected(true);
	event();
	zassert_false(running);
	download(echo, sizeof(echo));
	start("ready");
	/* A normal disconnect leaves the pending Python input and VM alive. */
	connected(false);
	struct application_status status;
	application_get_status(&status);
	zassert_equal(status.state, APPLICATION_RUNNING);
	connected(true);
	event();
	zassert_true(running);
	atomic_set(&reject_notifications, 2);
	const uint8_t input[] = {PBIO_PYBRICKS_COMMAND_WRITE_STDIN, 'B', 'L', 'E', '\n'};
	zassert_equal(command(input, sizeof(input)), sizeof(input));
	int64_t deadline = k_uptime_get() + 2000;
	do {
		event();
		zassert_true(k_uptime_get() < deadline);
	} while (running);
	zassert_not_null(strstr(output, "echo BLE\r\nfragment:"), "%s", output);
	char *fragment = strstr(output, "fragment:") + strlen("fragment:");
	zassert_equal(strspn(fragment, "x"), 96, "%s", output);
	zassert_equal(notifications_retried, 2);
	application_get_status(&status);
	zassert_equal(status.state, APPLICATION_RETURNED);
	zassert_equal(status.result, 0);

	download(spin, sizeof(spin));
	start("loop");
	const uint8_t run = PBIO_PYBRICKS_COMMAND_START_USER_PROGRAM;
	zassert_equal(command(&run, 1), BT_GATT_ERR(PBIO_PYBRICKS_ERROR_BUSY));
	const uint8_t stop = PBIO_PYBRICKS_COMMAND_STOP_USER_PROGRAM;
	zassert_equal(command(&stop, 1), 1);
	application_get_status(&status);
	zassert_equal(status.state, APPLICATION_STOPPED);
	zassert_equal(status.cleanup_error, 0);
	/* Repeating a complete download uses the same retained RAM image. */
	start("loop");
	zassert_equal(command(&stop, 1), 1);
	connected(false);
	application_get_status(&status);
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
	zassert_false(status.upload_ready);
#else
	zassert_true(status.upload_ready);
#endif
	zassert_equal(atomic_get(&refs), 0);
	k_msgq_purge(&events);
	connected(true);
	event();
	start("loop");
	zassert_equal(command(&stop, 1), 1);
	/* Only incomplete staging expires with its connection. The next peer
	 * cannot finalize it and must be able to make a fresh valid download. */
	const uint8_t begin[] = {PBIO_PYBRICKS_COMMAND_WRITE_USER_PROGRAM_META, 0, 0, 0, 0};
	const uint8_t fragment_write[] = {PBIO_PYBRICKS_COMMAND_WRITE_USER_RAM, 0, 0, 0, 0, 1};
	zassert_equal(command(begin, sizeof(begin)), sizeof(begin));
	zassert_equal(command(fragment_write, sizeof(fragment_write)), sizeof(fragment_write));
	connected(false);
	application_get_status(&status);
	zassert_equal(status.upload_bytes, 0);
	zassert_false(status.upload_ready);
	k_msgq_purge(&events);
	connected(true);
	event();
	const uint8_t finish[] = {PBIO_PYBRICKS_COMMAND_WRITE_USER_PROGRAM_META, 1, 0, 0, 0};
	zassert_equal(command(finish, sizeof(finish)),
		BT_GATT_ERR(PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED));
	download(echo, sizeof(echo));
	start("ready");
	zassert_equal(command(&stop, 1), 1);
	connected(false);
	zassert_equal(atomic_get(&refs), 0);
	size_t unused;
	zassert_ok(k_thread_stack_space_get(&rx_queue.thread, &unused));
	printk("Synthetic GATT command stack: %zu bytes unused\n", unused);
}

ZTEST(python_ble, test_native_execution)
{
	atomic_set(&reject_notifications, 0);
	k_msgq_purge(&events);
	connected(true);
	event();
	download(native, sizeof(native));
	for (unsigned int i = 0; i < 2; i++) {
		start("native and viper 42 4950");
		int64_t deadline = k_uptime_get() + 2000;
		while (running) {
			event();
			zassert_true(k_uptime_get() < deadline);
		}
		struct application_status status;
		application_get_status(&status);
		zassert_equal(status.result, 0);
		zassert_equal(status.cleanup_error, 0);
	}
	connected(false);
}

static void fill_output(void)
{
	atomic_set(&reject_notifications, 0);
	k_msgq_purge(&events);
	connected(true);
	event();
	download(flood, sizeof(flood));
	start("ready");
	atomic_set(&reject_notifications, 1000);
	const uint8_t input[] = {PBIO_PYBRICKS_COMMAND_WRITE_STDIN, '\n'};
	zassert_equal(command(input, sizeof(input)), sizeof(input));
	struct application_status status;
	int64_t deadline = k_uptime_get() + 1000;
	do {
		k_msleep(1);
		application_get_status(&status);
		zassert_true(k_uptime_get() < deadline);
		zassert_equal(status.state, APPLICATION_RUNNING);
	} while (status.output_bytes < 1024);
	zassert_equal(status.output_dropped, 0);
}

ZTEST(python_ble, test_output_flow_control)
{
	fill_output();
	atomic_set(&reject_notifications, 0);
	int64_t deadline = k_uptime_get() + 6000;
	do {
		event();
		zassert_true(k_uptime_get() < deadline);
	} while (running);
	const char *text = strstr(output, "0123456789");
	zassert_not_null(text, "%s", output);
	zassert_equal(strlen(text), 4093);
	for (size_t i = 0; i < 409; i++) {
		zassert_mem_equal(text + 10 * i, "0123456789", 10);
	}
	zassert_str_equal(text + 4090, "!\r\n");
	struct application_status status;
	application_get_status(&status);
	zassert_equal(status.result, 0);
	zassert_equal(status.output_dropped, 0);
	connected(false);

	fill_output();
	const uint8_t stop = PBIO_PYBRICKS_COMMAND_STOP_USER_PROGRAM;
	zassert_equal(command(&stop, 1), 1);
	application_get_status(&status);
	zassert_equal(status.state, APPLICATION_STOPPED);
	zassert_equal(status.cleanup_error, 0);
	connected(false);
	atomic_set(&reject_notifications, 0);

	fill_output();
	const uint8_t interrupt[] = {PBIO_PYBRICKS_COMMAND_WRITE_STDIN, 3};
	zassert_equal(command(interrupt, sizeof(interrupt)), sizeof(interrupt));
	deadline = k_uptime_get() + 1000;
	do {
		k_msleep(1);
		application_get_status(&status);
		zassert_true(k_uptime_get() < deadline);
	} while (status.state == APPLICATION_RUNNING);
	/* Catchable interruption can finish without draining a blocked write. */
	zassert_equal(status.state, APPLICATION_RETURNED);
	zassert_equal(status.result, 0);
	zassert_equal(status.output_dropped, 0);
	connected(false);

	fill_output();
	connected(false);
	atomic_set(&reject_notifications, 0);
	deadline = k_uptime_get() + 1000;
	do {
		k_msleep(1);
		application_get_status(&status);
		zassert_true(k_uptime_get() < deadline);
	} while (status.state == APPLICATION_RUNNING);
	/* Normal detach releases the pending write; it never stops the program. */
	zassert_equal(status.state, APPLICATION_RETURNED);
	zassert_equal(status.result, 0);
	zassert_true(status.output_dropped > 0);

	fill_output();
	const struct bt_gatt_attr *ccc = &brickphyr_terminal.attrs[3];
	struct bt_gatt_ccc_managed_user_data *cfg = ccc->user_data;
	zassert_equal(cfg->cfg_write(peer, ccc, BT_GATT_CCC_NOTIFY), 2);
	zassert_equal(host_ble_status(), HOST_BLE_NATIVE);
	deadline = k_uptime_get() + 1000;
	do {
		k_msleep(1);
		application_get_status(&status);
		zassert_true(k_uptime_get() < deadline);
	} while (status.state == APPLICATION_RUNNING);
	/* The same physical link can select native management: the old Pybricks
	 * subscription must no longer hold up the autonomous application's log. */
	zassert_equal(status.state, APPLICATION_RETURNED);
	zassert_equal(status.result, 0);
	zassert_true(status.output_dropped > 0);
	connected(false);
	atomic_set(&reject_notifications, 0);
	uint8_t state[8];
	do {
		host_interface_status(state);
		k_msleep(1);
		zassert_true(k_uptime_get() < deadline);
	} while (state[2] || !state[3]);
}

static void smp_exchange(const void *request, size_t length)
{
	const struct bt_gatt_attr *attr = bt_gatt_find_by_uuid(NULL, 0, SMP_BT_CHR_UUID);
	zassert_not_null(attr);
	zassert_equal(gatt_write(attr, request, length), length);
	uint8_t response[384];
	size_t received = 0, expected = 8;
	while (received < expected) {
		struct event part;
		zassert_ok(k_msgq_get(&events, &part, K_SECONDS(1)));
		zassert_true(received + part.length <= sizeof(response));
		memcpy(response + received, part.data, part.length);
		received += part.length;
		if (received >= 8) {
			expected = 8 + sys_get_be16(response + 2);
			zassert_true(expected <= sizeof(response));
		}
	}
	const uint8_t *header = request;
	zassert_equal(received, expected);
	zassert_equal(response[0], header[0] + 1);
	zassert_mem_equal(response + 4, header + 4, 4);
	if (header[0] == 0x0a) {
		ZCBOR_STATE_D(state, 2, response + 8, expected - 8, 1, 0);
		zassert_true(zcbor_map_start_decode(state) && zcbor_map_end_decode(state));
	}
}

ZTEST(python_ble, test_native_smp_output_attachment)
{
	atomic_set(&reject_notifications, 0);
	k_msgq_purge(&events);
	connected(true);
	event();
	download(flood, sizeof(flood));
	start("ready");
	const struct bt_gatt_attr *ccc = &pybricks_service.attrs[3];
	struct bt_gatt_ccc_managed_user_data *cfg = ccc->user_data;
	cfg->cfg_changed(ccc, 0);
	k_msgq_purge(&events);
	/* Standard SMP v2 requests encoded by brickphyrdev's public message classes. */
	const uint8_t attach[] = "\x0a\x00\x00\x0b\x00\x41\x00\x04"
		"\xa1\x68" "attached" "\xf5";
	const uint8_t input[] = "\x0a\x00\x00\x08\x00\x41\x01\x0a"
		"\xa1\x64" "data" "\x41\x0a";
	const uint8_t query[] = "\x08\x00\x00\x01\x00\x41\x02\x00\xa0";
	const uint8_t interrupt[] = "\x0a\x00\x00\x01\x00\x41\x03\x0b\xa0";
	smp_exchange(attach, sizeof(attach) - 1);
	smp_exchange(input, sizeof(input) - 1);
	struct application_status status;
	int64_t deadline = k_uptime_get() + 1000;
	do {
		k_msleep(1);
		application_get_status(&status);
		zassert_equal(status.state, APPLICATION_RUNNING);
		zassert_true(k_uptime_get() < deadline);
	} while (status.output_bytes < 1024);
	for (unsigned int i = 0; i < 3; i++) {
		smp_exchange(query, sizeof(query) - 1);
		k_msleep(1);
		application_get_status(&status);
		zassert_equal(status.state, APPLICATION_RUNNING);
		zassert_equal(status.output_bytes, 1024);
		zassert_equal(status.output_dropped, 0);
	}
	smp_exchange(interrupt, sizeof(interrupt) - 1);
	do {
		k_msleep(1);
		application_get_status(&status);
		zassert_true(k_uptime_get() < deadline);
	} while (status.state == APPLICATION_RUNNING);
	zassert_equal(status.state, APPLICATION_RETURNED);
	zassert_equal(status.result, 0);
	connected(false);
}

static void terminal_send(const char *text)
{
	size_t length = strlen(text);
	while (length) {
		size_t count = MIN(length, sizeof(command_data));
		zassert_equal(gatt_write(&brickphyr_terminal.attrs[5], text, count), count);
		text += count;
		length -= count;
	}
}

static void terminal_until(const char *marker)
{
	output_size = 0;
	output[0] = 0;
	int64_t deadline = k_uptime_get() + 2000;
	while (!strstr(output, marker)) {
		struct event part;
		zassert_ok(k_msgq_get(&events, &part, K_SECONDS(1)), "%s", output);
		zassert_true(k_uptime_get() < deadline, "%s", output);
		zassert_true(output_size + part.length < sizeof(output));
		memcpy(output + output_size, part.data, part.length);
		output_size += part.length;
		output[output_size] = 0;
	}
}

ZTEST(python_ble, test_native_terminal_output)
{
	atomic_set(&negotiated_mtu, 23);
	atomic_set(&shell_max_notify, 0);
	atomic_set(&reject_notifications, 0);
	k_msgq_purge(&events);
	connected(true);
	event();
	const struct bt_gatt_attr *ccc = &brickphyr_terminal.attrs[3];
	struct bt_gatt_ccc_managed_user_data *cfg = ccc->user_data;
	zassert_equal(cfg->cfg_write(peer, ccc, BT_GATT_CCC_NOTIFY), 2);
	terminal_until("brickphyr> ");
	terminal_send("python\r");
	terminal_until(">>> ");
	for (unsigned int i = 0; i < 2; i++) {
		atomic_set(&reject_notifications, 1000);
		terminal_send(i ? "while True: print('x' * 128)\r\r" :
			"print('0123456789' * 409 + '!')\r");
		struct application_status status;
		int64_t deadline = k_uptime_get() + 1000;
		do {
			k_msleep(1);
			application_get_status(&status);
			zassert_equal(status.state, APPLICATION_RUNNING);
			zassert_true(k_uptime_get() < deadline);
		} while (status.output_bytes < 1024);
		zassert_equal(status.output_dropped, 0);
		if (i) {
			/* Explicit exit reaches the supervisor with stdout still blocked. */
			terminal_send("\x18");
			do {
				k_msleep(1);
				application_get_status(&status);
				zassert_true(k_uptime_get() < deadline);
			} while (status.state == APPLICATION_RUNNING);
			zassert_equal(status.state, APPLICATION_STOPPED);
			zassert_equal(status.cleanup_error, 0);
		}
		atomic_set(&reject_notifications, 0);
		terminal_until(i ? "brickphyr> " : ">>> ");
		if (!i) {
			const char *text = strstr(output, "\r\n0123456789");
			zassert_not_null(text, "%s", output);
			text += 2;
			for (size_t n = 0; n < 409; n++) {
				zassert_mem_equal(text + 10 * n, "0123456789", 10);
			}
			zassert_str_equal(text + 4090, "!\r\n>>> ");
			zassert_equal(atomic_get(&shell_max_notify), 20);
			/* A peer exchange raises the ATT MTU on this connection. */
			atomic_set(&negotiated_mtu, 64);
			atomic_set(&shell_max_notify, 0);
			terminal_send("print('x' * 128)\r");
			terminal_until(">>> ");
			char line[133] = "\r\n";
			memset(line + 2, 'x', 128);
			memcpy(line + 130, "\r\n", 3);
			zassert_not_null(strstr(output, line), "%s", output);
			zassert_equal(atomic_get(&shell_max_notify), 61);
		}
	}
	connected(false);
	atomic_set(&negotiated_mtu, 23);
	uint8_t state[8];
	int64_t deadline = k_uptime_get() + 1000;
	do {
		k_msleep(1);
		host_interface_status(state);
		zassert_true(k_uptime_get() < deadline);
	} while (state[2] || !state[3]);
}

static void usb_send(uint8_t type, const void *data, size_t length)
{
	uint8_t frame[HOST_PROTOCOL_FRAME_MAX];
	size_t size = pbio_cobs_encode_prefixed(type, data, length, frame);
	zassert_equal(uart_emul_put_rx_data(uart, frame, size), size);
}

static void message_event(bool usb)
{
	if (!usb) {
		event();
		return;
	}
	uint8_t frame[HOST_PROTOCOL_FRAME_MAX], data[HOST_PROTOCOL_PAYLOAD_MAX], byte, type;
	size_t size = 0;
	int64_t deadline = k_uptime_get() + 1000;
	for (;;) {
		if (!uart_emul_get_tx_data(uart, &byte, 1)) {
			zassert_true(k_uptime_get() < deadline, "USB event missing: %s", output);
			k_msleep(1);
			continue;
		}
		if (byte != PBIO_COBS_DELIMITER) {
			zassert_true(size < sizeof(frame));
			frame[size++] = byte;
			continue;
		}
		if (!size) continue;
		size_t count = pbio_cobs_decode_prefixed(frame, size, &type, data, sizeof(data));
		zassert_true(count > 0);
		size = 0;
		if (type == PBIO_PYBRICKS_IN_EP_MSG_RESPONSE) {
			zassert_equal(count, 5);
			zassert_equal(sys_get_le32(data + 1), 0);
		} else {
			zassert_equal(type, PBIO_PYBRICKS_IN_EP_MSG_EVENT);
			record_event(data, count, false);
			return;
		}
	}
}

static void message_command(bool usb, const void *data, size_t length)
{
	if (usb) {
		uint8_t packet[64] = {1};
		zassert_true(length < sizeof(packet));
		memcpy(packet + 1, data, length);
		usb_send(PBIO_PYBRICKS_OUT_EP_MSG_COMMAND, packet, length + 1);
	} else {
		zassert_equal(command(data, length), length);
	}
}

static void message_until(bool usb, const char *marker)
{
	int64_t deadline = k_uptime_get() + 2000;
	while (!strstr(output, marker)) {
		message_event(usb);
		zassert_true(k_uptime_get() < deadline, "%s", output);
	}
}

static uint8_t advertisement[31];
static size_t advertisement_size;
static int8_t advertisement_rssi;
static void receive_advertisement(struct k_work *work)
{
	struct net_buf_simple packet;
	net_buf_simple_init_with_data(&packet, advertisement, advertisement_size);
	zassert_not_null(scan_callback);
	scan_callback(BT_ADDR_LE_ANY, advertisement_rssi, BT_GAP_ADV_TYPE_ADV_IND, &packet);
	k_sem_give(&command_done);
}
static K_WORK_DEFINE(advertisement_work, receive_advertisement);

static void advertise_sample(const void *data, size_t length, int8_t rssi)
{
	zassert_true(length <= sizeof(advertisement));
	memcpy(advertisement, data, length);
	advertisement_size = length;
	advertisement_rssi = rssi;
	k_work_submit_to_queue(&rx_queue, &advertisement_work);
	zassert_ok(k_sem_take(&command_done, K_SECONDS(1)));
}

static void radio_action(const char *action, const char *marker)
{
	uint8_t request[20] = {PBIO_PYBRICKS_COMMAND_WRITE_STDIN};
	size_t size = strlen(action);
	zassert_true(size + 2 <= sizeof(request));
	memcpy(request + 1, action, size);
	request[size + 1] = '\n';
	message_command(false, request, size + 2);
	message_until(false, marker);
}

ZTEST(python_ble, test_radio_observation_and_cleanup)
{
	static const uint8_t program[] = {
#include <radio.inc>
	};
	/* Literal reference-format packets, not generated by our decoder. The
	 * flags element verifies manufacturer data need not be first. */
	const uint8_t values[] = {2, 1, 6, 19, 0xff, 0x97, 3, 7,
		0x20, 0x40, 0x62, 0xd4, 0xfe, 0x84, 0, 0, 0xc0, 0x3f,
		0xa2, 'g', 'o', 0xc1, 0};
	const uint8_t empty[] = {4, 0xff, 0x97, 3, 8};
	const uint8_t bad[] = {5, 0xff, 0x97, 3, 7, 0}; /* Scalar with no value. */
	const uint8_t truncated[] = {31, 0xff, 0x97, 3, 7, 0};
	k_msgq_purge(&events);
	connected(true);
	download(program, sizeof(program));
	start("radio ready");
	zassert_equal(host_ble_status(), HOST_BLE_PYBRICKS);
	advertise_sample(values, sizeof(values), -60);
	advertise_sample(empty, sizeof(empty), -60);
	advertise_sample(truncated, sizeof(truncated), -10);
	radio_action("fresh", "fresh done");
	/* A peer repeating unchanged values is still present after a second. */
	k_msleep(700);
	advertise_sample(values, sizeof(values), -40);
	k_msleep(700);
	radio_action("refresh", "refresh done");
	advertise_sample(bad, sizeof(bad), -40);
	radio_action("bad", "bad done");
	k_msleep(1010);
	radio_action("expired", "expired done");
	radio_action("reopen", "reopen done");
	/* No explicit close: forced stop must retire the observer and its heap. */
	const uint8_t stop = PBIO_PYBRICKS_COMMAND_STOP_USER_PROGRAM;
	message_command(false, &stop, 1);
	int64_t deadline = k_uptime_get() + 1000;
	while (scan_callback) {
		k_msleep(1);
		zassert_true(k_uptime_get() < deadline);
	}
	start("radio ready");
	message_command(false, &stop, 1);
	while (scan_callback) {
		k_msleep(1);
		zassert_true(k_uptime_get() < deadline);
	}
	zassert_equal(atomic_get(&scan_starts), atomic_get(&scan_stops));
	connected(false);
}

ZTEST(python_ble, test_app_data_transports_and_lifetime)
{
	static const uint8_t program[] = {
#include <messaging.inc>
	};
	k_msgq_purge(&events);
	connected(true);
	download(program, sizeof(program));
	for (unsigned int usb = 0; usb < 2; usb++) {
		output_size = message_size = 0;
		output[0] = 0;
		if (usb) {
			zassert_ok(host_interface_select(HOST_PYBRICKS));
			uart_emul_flush_tx_data(uart);
			host_connection_changed(true);
			const uint8_t subscribe = 1;
			usb_send(PBIO_PYBRICKS_OUT_EP_MSG_SUBSCRIBE, &subscribe, 1);
		}
		const uint8_t start = PBIO_PYBRICKS_COMMAND_START_USER_PROGRAM;
		message_command(usb, &start, 1);
		message_until(usb, "ready");
		zassert_equal(message_size, 3);
		zassert_mem_equal(message_data, "\0\1\11", 3);
		const uint8_t write[] = {PBIO_PYBRICKS_COMMAND_WRITE_APP_DATA, 0, 0,
			'a', 'b', 'c', 'd', 'e', 'f'};
		message_command(usb, write, sizeof(write));
		if (!usb) {
			const uint8_t bad[] = {PBIO_PYBRICKS_COMMAND_WRITE_APP_DATA, 6, 0, 1};
			zassert_equal(command(bad, sizeof(bad)),
				BT_GATT_ERR(PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED));
		}
		const uint8_t read[] = "\x06" "read\n";
		message_command(usb, read, sizeof(read) - 1);
		message_until(usb, "read done");
		zassert_equal(message_size, 5);
		zassert_mem_equal(message_data, "\1\11\3ok", 5);
		const uint8_t large[] = "\x06" "large\n";
		if (!usb) {
			message_command(false, large, sizeof(large) - 1);
			message_until(false, "too large");
			atomic_set(&negotiated_mtu, 64);
		}
		message_command(usb, large, sizeof(large) - 1);
		message_until(usb, "large done");
		zassert_equal(message_size, 60);
		for (size_t i = 0; i < message_size; i++) zassert_equal(message_data[i], i);
		const uint8_t replace[] = "\x06" "replace\n";
		message_command(usb, replace, sizeof(replace) - 1);
		message_until(usb, "replace done");
		zassert_equal(message_size, 5);
		zassert_mem_equal(message_data, "final", 5);
		const uint8_t close[] = "\x06" "close\n";
		message_command(usb, close, sizeof(close) - 1);
		message_until(usb, "close done");
		zassert_equal(message_size, 2);
		zassert_mem_equal(message_data, "\0\2", 2);
		const uint8_t stop = PBIO_PYBRICKS_COMMAND_STOP_USER_PROGRAM;
		message_command(usb, &stop, 1);
		int64_t deadline = k_uptime_get() + 1000;
		do {
			message_event(usb);
			zassert_true(k_uptime_get() < deadline);
		} while (running);
		struct application_status status;
		application_get_status(&status);
		zassert_equal(status.state, APPLICATION_STOPPED);
		zassert_equal(status.cleanup_error, 0);
		if (usb) host_connection_changed(false);
		else connected(false);
		uint8_t state[8];
		do {
			k_msleep(1);
			host_interface_status(state);
			zassert_true(k_uptime_get() < deadline);
		} while (state[2] || !state[3]);
	}
	/* Force-stop freed the singleton; detach aborts an outstanding send but
	 * leaves its Python application alive, able to reopen after reconnect. */
	atomic_set(&negotiated_mtu, 23);
	k_msgq_purge(&events);
	connected(true);
	start("ready");
	atomic_set(&reject_app_notifications, 1);
	unsigned int previous = app_notifications_retried;
	const uint8_t blocked[] = "\x06" "blocked\n";
	message_command(false, blocked, sizeof(blocked) - 1);
	int64_t deadline = k_uptime_get() + 1000;
	while (app_notifications_retried == previous) {
		k_msleep(1);
		zassert_true(k_uptime_get() < deadline);
	}
	connected(false);
	atomic_set(&reject_app_notifications, 0);
	struct application_status status;
	do {
		k_msleep(1);
		application_get_status(&status);
		zassert_equal(status.state, APPLICATION_RUNNING);
		zassert_true(k_uptime_get() < deadline);
	} while (status.output_bytes < strlen("disconnected\r\n"));
	k_msgq_purge(&events);
	connected(true);
	const uint8_t reopen[] = "\x06" "reopen\n";
	message_command(false, reopen, sizeof(reopen) - 1);
	message_until(false, "reopened");
	const uint8_t stop = PBIO_PYBRICKS_COMMAND_STOP_USER_PROGRAM;
	message_command(false, &stop, 1);
	connected(false);
}

ZTEST(python_ble, test_usb_stop_with_unread_output)
{
	/* Cross-transport start must use the completed upload or stored slot. */
	connected(true);
	download(flood, sizeof(flood));
	connected(false);
	zassert_ok(host_interface_select(HOST_PYBRICKS));
	host_connection_changed(true);
	const uint8_t subscribe = 1;
	usb_send(PBIO_PYBRICKS_OUT_EP_MSG_SUBSCRIBE, &subscribe, 1);
	const uint8_t start[] = {1, PBIO_PYBRICKS_COMMAND_START_USER_PROGRAM};
	usb_send(PBIO_PYBRICKS_OUT_EP_MSG_COMMAND, start, sizeof(start));
	const uint8_t input[] = {2, PBIO_PYBRICKS_COMMAND_WRITE_STDIN, '\n'};
	usb_send(PBIO_PYBRICKS_OUT_EP_MSG_COMMAND, input, sizeof(input));
	struct application_status status;
	int64_t deadline = k_uptime_get() + 1000;
	do {
		k_msleep(1);
		application_get_status(&status);
		zassert_true(k_uptime_get() < deadline);
	} while (status.output_bytes < 1024);
	zassert_equal(status.state, APPLICATION_RUNNING);
	zassert_equal(status.output_dropped, 0);
	/* Do not read even the command replies: the first STOP must still reach
	 * the supervisor behind an unsent stdout event and a full UART FIFO. */
	const uint8_t stop[] = {3, PBIO_PYBRICKS_COMMAND_STOP_USER_PROGRAM};
	usb_send(PBIO_PYBRICKS_OUT_EP_MSG_COMMAND, stop, sizeof(stop));
	deadline = k_uptime_get() + 1000;
	do {
		k_msleep(1);
		application_get_status(&status);
		zassert_true(k_uptime_get() < deadline);
	} while (status.state == APPLICATION_RUNNING);
	zassert_equal(status.state, APPLICATION_STOPPED);
	zassert_equal(status.cleanup_error, 0);
	host_connection_changed(false);
	uart_emul_flush_tx_data(uart);
	uint8_t state[8];
	do {
		host_interface_status(state);
		k_msleep(1);
		zassert_true(k_uptime_get() < deadline);
	} while (state[2] || !state[3]);
}

#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
static const uint8_t padded[] = {
#include <padded.inc>
};

ZTEST(python_ble, test_slot_capacity_preserves_previous_image)
{
	/* A normal named program shares the store and survives client-slot work. */
	const uint8_t named[] = "\xa1\x67main.py\x4a" "print(42)\n";
	size_t next;
	zassert_ok(application_upload(APPLICATION_FILES, 0, sizeof(named) - 1,
		named, sizeof(named) - 1, &next));
	zassert_ok(app_store_publish_upload("named"));
	bool full = false;
	unsigned int last = 0;
	for (unsigned int slot = 1; slot <= 4; slot++) {
		zassert_ok(host_protocol_select_slot(slot));
		k_msgq_purge(&events);
		connected(true);
		event();
		download(echo, sizeof(echo));
		last = slot;
		ssize_t result = transfer(padded, sizeof(padded));
		if (result == 5) {
			start("padded");
			while (running) {
				event();
			}
		} else {
			zassert_equal(result, BT_GATT_ERR(PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED));
			/* Exhaustion cannot remove the smaller program being replaced. */
			start("ready");
			const uint8_t stop = PBIO_PYBRICKS_COMMAND_STOP_USER_PROGRAM;
			zassert_equal(command(&stop, 1), 1);
			full = true;
		}
		connected(false);
		if (full) {
			break;
		}
	}
	zassert_true(full, "Synthetic store did not reach capacity");
	for (unsigned int slot = 1; slot <= last; slot++) {
		char name[5];
		snprintk(name, sizeof(name), "@%u", slot);
		zassert_ok(app_store_remove(name));
	}
	zassert_ok(app_store_run("named"));
	struct application_status status;
	int64_t deadline = k_uptime_get() + 1000;
	do {
		k_msleep(1);
		application_get_status(&status);
		zassert_true(k_uptime_get() < deadline);
	} while (status.state == APPLICATION_RUNNING);
	uint8_t text[16];
	zassert_equal(application_output(text, sizeof(text)), 4);
	zassert_mem_equal(text, "42\r\n", 4);
	zassert_ok(app_store_remove("named"));
	zassert_ok(host_protocol_select_slot(0));
}

ZTEST(python_ble, test_slots_preserve_installed_programs)
{
	zassert_ok(app_store_status());
	zassert_ok(host_protocol_select_slot(1));
	k_msgq_purge(&events);
	connected(true);
	event();
	download(echo, sizeof(echo));
	connected(false);
	zassert_ok(host_protocol_select_slot(2));
	k_msgq_purge(&events);
	connected(true);
	event();
	download(spin, sizeof(spin));
	start("loop");
	zassert_equal(host_protocol_select_slot(1), -EBUSY);
	const uint8_t stop = PBIO_PYBRICKS_COMMAND_STOP_USER_PROGRAM;
	zassert_equal(command(&stop, 1), 1);
	const uint8_t begin[] = {PBIO_PYBRICKS_COMMAND_WRITE_USER_PROGRAM_META, 0, 0, 0, 0};
	zassert_equal(command(begin, sizeof(begin)), sizeof(begin));
	zassert_equal(host_protocol_select_slot(1), -EBUSY);
	const uint8_t bad[] = {PBIO_PYBRICKS_COMMAND_WRITE_USER_RAM, 0, 0, 0, 0, 1};
	zassert_equal(command(bad, sizeof(bad)), sizeof(bad));
	connected(false);
	/* Disconnect discards incomplete RAM staging, not the existing slot. */
	application_poll();
	struct application_status status;
	application_get_status(&status);
	zassert_false(status.upload_ready);
	zassert_equal(status.upload_bytes, 0);
	zassert_ok(host_protocol_select_slot(2));
	k_msgq_purge(&events);
	connected(true);
	event();
	start("loop");
	zassert_equal(command(&stop, 1), 1);
	/* A completed but malformed replacement also leaves the slot intact. */
	zassert_equal(command(begin, sizeof(begin)), sizeof(begin));
	zassert_equal(command(bad, sizeof(bad)), sizeof(bad));
	const uint8_t finish[] = {PBIO_PYBRICKS_COMMAND_WRITE_USER_PROGRAM_META, 1, 0, 0, 0};
	zassert_equal(command(finish, sizeof(finish)),
		BT_GATT_ERR(PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED));
	start("loop");
	zassert_equal(command(&stop, 1), 1);
	connected(false);
	zassert_ok(host_protocol_select_slot(1));
	k_msgq_purge(&events);
	connected(true);
	event();
	start("ready");
	zassert_equal(command(&stop, 1), 1);
	connected(false);
	zassert_equal(app_store_publish("@1", 0), -EACCES);
	struct app_file entry;
	size_t available;
	zassert_ok(app_store_next("@0", &entry, &available));
	zassert_str_equal(entry.name, "@1");
	zassert_ok(app_store_next("@1", &entry, &available));
	zassert_str_equal(entry.name, "@2");
	zassert_ok(app_store_remove("@2"));
	zassert_equal(app_store_run("@2"), -ENOENT);
	zassert_ok(host_protocol_select_slot(0));
}
#endif

static void report_stack(const struct k_thread *thread, void *unused)
{
	const char *name = k_thread_name_get((k_tid_t)thread);
	if (name && !strcmp(name, "pybricks_store")) {
		size_t space;
		zassert_ok(k_thread_stack_space_get(thread, &space));
		printk("Pybricks store stack: %zu bytes unused\n", space);
	}
}

static void teardown(void *unused)
{
	atomic_set(&supervisor_stop, 1);
	zassert_ok(k_thread_join(&supervisor, K_SECONDS(1)));
	k_thread_foreach(report_stack, NULL);
}

ZTEST_SUITE(python_ble, NULL, setup, NULL, NULL, teardown);
