// SPDX-License-Identifier: Apache-2.0
#include "host_transport.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
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
	starts++;
	return 0;
}

int __wrap_bt_le_adv_stop(void)
{
	stops++;
	/* Stopping advertising can recycle its reserved connection object. */
	STRUCT_SECTION_FOREACH(bt_conn_cb, callback) {
		if (callback->recycled) {
			callback->recycled();
		}
	}
	return 0;
}

/* No connection callbacks are generated here; ownership has its own tests. */
int host_ble_acquire(void)
{
	zassert_unreachable("Unexpected connection");
	return -EBUSY;
}

void host_ble_release(bool exit_compatibility)
{
	zassert_unreachable("Unexpected disconnection");
}

/* This radio lifecycle fixture never opens a terminal or emits GATT input. */
int host_ble_shell_open(void (*tx_ready)(void), uint32_t *session)
{
	zassert_unreachable("Unexpected terminal subscription");
	return -ENOTCONN;
}
void host_ble_shell_close(uint32_t session) { zassert_unreachable(); }
int host_ble_shell_receive(uint32_t session, const void *data, size_t length)
{
	zassert_unreachable();
	return -ENOTCONN;
}
size_t host_ble_shell_peek(uint32_t session, void *data, size_t capacity)
{
	zassert_unreachable();
	return 0;
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

ZTEST(radio, test_startup_control_and_failure)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(k_sem_take(&enable_entered, K_SECONDS(1)));
	expect_status("Bluetooth: starting");
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
	zassert_equal(enables, 1);
	zassert_equal(starts, 1);
	zassert_equal(stops, 1);
	zassert_equal(disables, 1);

	zassert_ok(shell_execute_cmd(shell, "bluetooth on"));
	zassert_ok(shell_execute_cmd(shell, "bluetooth on"));
	expect_status("Bluetooth: on");
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
	zassert_equal(enables, 3);
	zassert_equal(starts, 2);
	zassert_equal(stops, 2);
	zassert_equal(disables, 2);

	host_ble_shutdown();
	zassert_ok(k_sem_take(&power_off_done, K_SECONDS(1)), "Failed radio prevented power off");
	host_ble_shutdown();
	zassert_equal(k_sem_take(&power_off_done, K_MSEC(1)), -EAGAIN);
	expect_status("initialization failed:");
	zassert_equal(shell_execute_cmd(shell, "bluetooth on"), -ESHUTDOWN);
	zassert_equal(enables, 3);
}

ZTEST_SUITE(radio, NULL, NULL, NULL, NULL, NULL);
