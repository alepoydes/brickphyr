// SPDX-License-Identifier: Apache-2.0
#include "host_transport.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

/* Replace only the radio operations. The production workqueues, lifecycle and
 * Shell commands remain intact; no real controller or host adapter is used. */
static K_SEM_DEFINE(enable_entered, 0, 1);
static K_SEM_DEFINE(enable_continue, 0, 1);
static K_SEM_DEFINE(probe_done, 0, 1);
static K_SEM_DEFINE(off_done, 0, 1);
static K_SEM_DEFINE(power_off_done, 0, 1);
static unsigned int enables, disables, starts, stops;
static int enable_error, off_result;
static K_SEM_DEFINE(disconnect_entered, 0, 1);
static K_SEM_DEFINE(disconnect_continue, 0, 1);
static K_SEM_DEFINE(peer_released, 0, 3);
static uintptr_t fake_peers[3];
static struct bt_conn *const peer = (void *)&fake_peers[0];
static struct bt_conn *const other = (void *)&fake_peers[1];
static struct bt_conn *const third = (void *)&fake_peers[2];
static atomic_t peer_refs[3];
static uint8_t peer_roles[3] = {
	BT_CONN_ROLE_PERIPHERAL, BT_CONN_ROLE_CENTRAL, BT_CONN_ROLE_PERIPHERAL,
};
static struct bt_conn *disconnect_current;
static atomic_t advertising_active;
static struct k_work_q *radio_work_queue;
static K_SEM_DEFINE(radio_drained, 0, 1);
static bool management_owned, terminal_closed;
static void (*terminal_output_ready)(void);
static int disconnect_error;
static int terminal_open_error;

static size_t peer_index(struct bt_conn *conn)
{
	if (conn == peer) return 0;
	if (conn == other) return 1;
	if (conn == third) return 2;
	zassert_unreachable("Unknown synthetic connection");
	return 0;
}

void __real_k_work_queue_start(struct k_work_q *queue, k_thread_stack_t *stack,
	size_t stack_size, int prio, const struct k_work_queue_config *cfg);

void __wrap_k_work_queue_start(struct k_work_q *queue, k_thread_stack_t *stack,
	size_t stack_size, int prio, const struct k_work_queue_config *cfg)
{
	if (cfg && cfg->name && !strcmp(cfg->name, "host_ble")) radio_work_queue = queue;
	__real_k_work_queue_start(queue, stack, stack_size, prio, cfg);
}

static void radio_barrier(struct k_work *work)
{
	k_sem_give(&radio_drained);
}
static K_WORK_DEFINE(radio_barrier_work, radio_barrier);

static void drain_radio_worker(void)
{
	zassert_not_null(radio_work_queue);
	zassert_true(k_work_submit_to_queue(radio_work_queue, &radio_barrier_work) > 0);
	zassert_ok(k_sem_take(&radio_drained, K_SECONDS(1)));
}

int hub_bluetooth_power_off(void)
{
	zassert_not_equal(k_current_get(), &k_sys_work_q.thread);
	k_sem_give(&power_off_done);
	return 0;
}

int __wrap_bt_enable(bt_ready_cb_t cb)
{
	zassert_is_null(cb);
	zassert_not_equal(k_current_get(), &k_sys_work_q.thread);
	if (++enables == 1) {
		k_sem_give(&enable_entered);
		k_sem_take(&enable_continue, K_FOREVER);
	}
	return enable_error;
}

int __wrap_bt_disable(void)
{
	disables++;
	return 0;
}

int __wrap_bt_le_adv_start(const struct bt_le_adv_param *param,
	const struct bt_data *ad, size_t ad_len, const struct bt_data *sd, size_t sd_len)
{
	if (!atomic_cas(&advertising_active, 0, 1)) return -EALREADY;
	starts++;
	return 0;
}

int __wrap_bt_le_adv_stop(void)
{
	stops++;
	atomic_clear(&advertising_active);
	/* Stopping advertising can recycle its reserved connection object. */
	STRUCT_SECTION_FOREACH(bt_conn_cb, callback) {
		if (callback->recycled) {
			callback->recycled();
		}
	}
	return 0;
}

struct bt_conn *__wrap_bt_conn_ref(struct bt_conn *conn)
{
	atomic_inc(&peer_refs[peer_index(conn)]);
	return conn;
}

void __wrap_bt_conn_unref(struct bt_conn *conn)
{
	atomic_val_t previous = atomic_dec(&peer_refs[peer_index(conn)]);
	zassert_true(previous > 0);
	if (previous == 1) {
		k_sem_give(&peer_released);
	}
}

int __wrap_bt_conn_disconnect(struct bt_conn *conn, uint8_t reason)
{
	peer_index(conn);
	zassert_not_equal(k_current_get(), &k_sys_work_q.thread);
	disconnect_current = conn;
	k_sem_give(&disconnect_entered);
	zassert_ok(k_sem_take(&disconnect_continue, K_SECONDS(1)));
	return disconnect_error;
}

int __wrap_bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info)
{
	memset(info, 0, sizeof(*info));
	info->type = BT_CONN_TYPE_LE;
	info->role = peer_roles[peer_index((struct bt_conn *)conn)];
	info->state = BT_CONN_STATE_CONNECTED;
	return 0;
}

bool __wrap_bt_gatt_is_subscribed(struct bt_conn *conn,
	const struct bt_gatt_attr *attr, uint16_t ccc)
{
	zassert_equal(conn, peer);
	return true;
}

int __wrap_bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params)
{
	zassert_equal(conn, peer);
	return -EIO;
}

uint16_t __wrap_bt_gatt_get_mtu(struct bt_conn *conn)
{
	zassert_equal(conn, peer);
	return 23;
}

/* Ownership itself has production-transport tests; this fixture drives the
 * GATT adapter and its scheduling with a synthetic connection/controller. */
bool host_ble_request_begin(void) { return management_owned; }
void host_ble_request_end(void) {}

int host_ble_acquire(void)
{
	if (management_owned) return -EBUSY;
	management_owned = true;
	return 0;
}

void host_ble_release(bool exit_compatibility)
{
	zassert_true(management_owned);
	management_owned = false;
}

int host_ble_shell_open(void (*tx_ready)(void), uint32_t *session)
{
	terminal_output_ready = tx_ready;
	*session = 1;
	return terminal_open_error;
}
void host_ble_shell_close(uint32_t session)
{
	zassert_equal(session, 1);
	terminal_closed = true;
}
int host_ble_shell_receive(uint32_t session, const void *data, size_t length)
{
	zassert_unreachable();
	return -ENOTCONN;
}
size_t host_ble_shell_peek(uint32_t session, void *data, size_t capacity)
{
	zassert_equal(session, 1);
	zassert_true(capacity > 0);
	*(uint8_t *)data = 'x';
	return 1;
}
void host_ble_shell_consume(uint32_t session, size_t length) { zassert_unreachable(); }

uint16_t host_hub_variant(void)
{
	return 1;
}

static void probe(struct k_work *work)
{
	k_sem_give(&probe_done);
}
static K_WORK_DEFINE(probe_work, probe);

static void turn_off(void *a, void *b, void *c)
{
	off_result = shell_execute_cmd(shell_backend_dummy_get_ptr(), "bluetooth off");
	k_sem_give(&off_done);
}
static struct k_thread off_thread;
static K_THREAD_STACK_DEFINE(off_stack, 4096);

static void expect_status(const char *text)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();
	shell_backend_dummy_clear_output(shell);
	zassert_ok(shell_execute_cmd(shell, "bluetooth"));
	size_t size;
	const char *reply = shell_backend_dummy_get_output(shell, &size);
	zassert_not_null(strstr(reply, text), "%s", reply);
}

static void connection_event(struct bt_conn *conn, uint8_t err)
{
	if (!err && peer_roles[peer_index(conn)] == BT_CONN_ROLE_PERIPHERAL) {
		/* Connectable advertising is consumed when a peripheral link arrives. */
		atomic_clear(&advertising_active);
	}
	STRUCT_SECTION_FOREACH(bt_conn_cb, callback) {
		if (callback->connected) {
			callback->connected(conn, err);
		}
	}
}

static void disconnect_event(struct bt_conn *conn)
{
	STRUCT_SECTION_FOREACH(bt_conn_cb, callback) {
		if (callback->disconnected) {
			callback->disconnected(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
	}
}

static void recycle_connection(void)
{
	STRUCT_SECTION_FOREACH(bt_conn_cb, callback) {
		if (callback->recycled) callback->recycled();
	}
}

static void expect_responsive_disconnect(void)
{
	zassert_ok(k_sem_take(&disconnect_entered, K_SECONDS(1)));
	k_work_submit(&probe_work);
	zassert_ok(k_sem_take(&probe_done, K_MSEC(100)), "Disconnect blocked button/system work");
}

ZTEST(radio, test_startup_control_and_failure)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(k_sem_take(&enable_entered, K_SECONDS(1)));
	expect_status("Bluetooth: starting");
	zassert_equal(host_ble_status(), HOST_BLE_OFF);
	zassert_equal(shell_execute_cmd(shell, "bluetooth invalid"), -EINVAL);
	k_work_submit(&probe_work);
	zassert_ok(k_sem_take(&probe_done, K_MSEC(100)), "Radio blocked the system queue");

	/* An explicit off must join in-flight startup and remain off afterward. */
	k_thread_create(&off_thread, off_stack, K_THREAD_STACK_SIZEOF(off_stack),
		turn_off, NULL, NULL, NULL, 1, 0, K_NO_WAIT);
	zassert_equal(k_sem_take(&off_done, K_MSEC(1)), -EAGAIN);
	k_sem_give(&enable_continue);
	zassert_ok(k_sem_take(&off_done, K_SECONDS(1)));
	zassert_ok(k_thread_join(&off_thread, K_SECONDS(1)));
	zassert_ok(off_result);
	k_msleep(1);
	expect_status("Bluetooth: off");
	zassert_equal(host_ble_status(), HOST_BLE_OFF);
	zassert_equal(enables, 1);
	zassert_equal(starts, 1);
	zassert_equal(stops, 1);
	zassert_equal(disables, 1);

	zassert_ok(shell_execute_cmd(shell, "bluetooth on"));
	zassert_ok(shell_execute_cmd(shell, "bluetooth on"));
	expect_status("Bluetooth: on");
	zassert_equal(host_ble_status(), HOST_BLE_WAITING);
	/* A central link does not acquire the computer session or consume our
	 * connectable advertisement, including after its slot is recycled. */
	connection_event(other, 0);
	zassert_equal(host_ble_status(), HOST_BLE_WAITING);
	zassert_false(management_owned);
	disconnect_event(other);
	recycle_connection();
	drain_radio_worker();
	zassert_equal(starts, 2);
	zassert_equal(atomic_get(&peer_refs[1]), 0);
	connection_event(peer, 0);
	zassert_equal(host_ble_status(), HOST_BLE_CONNECTED);
	/* Recycling a central slot while a management connection is present must
	 * not restart advertising or disturb that owner. */
	connection_event(other, 0);
	disconnect_event(other);
	recycle_connection();
	drain_radio_worker();
	zassert_equal(starts, 2);
	zassert_true(management_owned);
	zassert_equal(host_ble_status(), HOST_BLE_CONNECTED);
	/* Shared Device Information reads do not select a protocol. */
	extern const struct bt_gatt_service_static device_information, pybricks_service;
	uint8_t value[20];
	const struct bt_gatt_attr *info = &device_information.attrs[2];
	zassert_true(info->read(peer, info, value, sizeof(value), 0) > 0);
	zassert_equal(host_ble_status(), HOST_BLE_CONNECTED);
	const struct bt_gatt_attr *caps = &pybricks_service.attrs[5];
	zassert_true(caps->read(peer, caps, value, sizeof(value), UINT16_MAX) < 0);
	zassert_equal(host_ble_status(), HOST_BLE_CONNECTED);
	zassert_true(caps->read(peer, caps, value, sizeof(value), 0) > 0);
	zassert_equal(host_ble_status(), HOST_BLE_PYBRICKS);
	extern const struct bt_gatt_service_static brickphyr_terminal;
	const struct bt_gatt_attr *ccc = &brickphyr_terminal.attrs[3];
	struct bt_gatt_ccc_managed_user_data *cfg = ccc->user_data;
	terminal_open_error = -EBUSY;
	zassert_true(cfg->cfg_write(peer, ccc, BT_GATT_CCC_NOTIFY) < 0);
	zassert_equal(host_ble_status(), HOST_BLE_PYBRICKS);
	terminal_open_error = 0;
	zassert_equal(cfg->cfg_write(peer, ccc, BT_GATT_CCC_NOTIFY), 2);
	zassert_equal(host_ble_status(), HOST_BLE_NATIVE);
	const struct bt_gatt_attr *py_ccc = &pybricks_service.attrs[3];
	struct bt_gatt_ccc_managed_user_data *py_cfg = py_ccc->user_data;
	zassert_equal(py_cfg->cfg_write(peer, py_ccc, BT_GATT_CCC_NOTIFY), 2);
	zassert_equal(host_ble_status(), HOST_BLE_NATIVE,
		      "Pybricks discovery must not recolor an open native terminal");
	terminal_output_ready();
	expect_responsive_disconnect();
	zassert_equal(disconnect_current, peer);
	zassert_true(terminal_closed);
	zassert_true(management_owned, "A disconnect request cannot release the session");
	/* The first HCI command is stalled. Both arriving peripheral peers must be
	 * queued independently, and repeated rejection of one must not add a ref. */
	peer_roles[1] = BT_CONN_ROLE_PERIPHERAL;
	connection_event(other, 0);
	connection_event(third, 0);
	connection_event(other, 0);
	zassert_equal(atomic_get(&peer_refs[1]), 1);
	zassert_equal(atomic_get(&peer_refs[2]), 1);
	zassert_true(management_owned);
	/* An actual disconnect may arrive while the command still waits for HCI. */
	disconnect_event(peer);
	zassert_false(management_owned);
	zassert_equal(host_ble_status(), HOST_BLE_WAITING);
	zassert_equal(atomic_get(&peer_refs[0]), 1, "Worker must retain the disconnected peer");
	k_sem_give(&disconnect_continue);
	zassert_ok(k_sem_take(&peer_released, K_SECONDS(1)));
	bool saw_other = false, saw_third = false;
	for (size_t i = 0; i < 2; i++) {
		zassert_ok(k_sem_take(&disconnect_entered, K_SECONDS(1)));
		if (disconnect_current == other) {
			zassert_false(saw_other);
			saw_other = true;
		} else {
			zassert_equal(disconnect_current, third);
			zassert_false(saw_third);
			saw_third = true;
		}
		k_sem_give(&disconnect_continue);
		zassert_ok(k_sem_take(&peer_released, K_SECONDS(1)));
	}
	zassert_true(saw_other && saw_third);
	zassert_equal(atomic_get(&peer_refs[0]), 0);
	zassert_equal(atomic_get(&peer_refs[1]), 0);
	zassert_equal(atomic_get(&peer_refs[2]), 0);
	connection_event(peer, 0);
	zassert_equal(host_ble_status(), HOST_BLE_CONNECTED, "New link must forget old profile");
	zassert_equal(py_cfg->cfg_write(peer, py_ccc, BT_GATT_CCC_NOTIFY), 2);
	zassert_equal(host_ble_status(), HOST_BLE_PYBRICKS);
	disconnect_event(peer);
	zassert_ok(k_sem_take(&peer_released, K_SECONDS(1)));
	zassert_ok(shell_execute_cmd(shell, "bluetooth off"));
	zassert_ok(shell_execute_cmd(shell, "bluetooth off"));
	k_msleep(1);
	zassert_equal(enables, 2);
	zassert_equal(starts, 2);
	zassert_equal(stops, 2);
	zassert_equal(disables, 2);

	enable_error = -ETIMEDOUT;
	zassert_equal(shell_execute_cmd(shell, "bluetooth on"), -ETIMEDOUT);
	enable_error = -EIO;
	zassert_equal(shell_execute_cmd(shell, "bluetooth on"), -ETIMEDOUT);
	zassert_equal(shell_execute_cmd(shell, "bluetooth off"), -ETIMEDOUT);
	expect_status("initialization failed:");
	zassert_equal(host_ble_status(), HOST_BLE_OFF);
	zassert_equal(enables, 3);
	zassert_equal(starts, 2);
	zassert_equal(stops, 2);
	zassert_equal(disables, 2);

	/* Failed-radio rejection uses the same asynchronous path and cannot
	 * replace the first lifecycle error with a disconnect failure. */
	disconnect_error = -EIO;
	connection_event(peer, 0);
	expect_responsive_disconnect();
	k_sem_give(&disconnect_continue);
	zassert_ok(k_sem_take(&peer_released, K_SECONDS(1)));
	expect_status("initialization failed: -110");
	zassert_false(management_owned);

	host_ble_shutdown();
	zassert_ok(k_sem_take(&power_off_done, K_SECONDS(1)), "Failed radio prevented power off");
	host_ble_shutdown();
	zassert_equal(k_sem_take(&power_off_done, K_MSEC(1)), -EAGAIN);
	expect_status("initialization failed:");
	zassert_equal(shell_execute_cmd(shell, "bluetooth on"), -ESHUTDOWN);
	zassert_equal(enables, 3);
}

ZTEST_SUITE(radio, NULL, NULL, NULL, NULL, NULL);
