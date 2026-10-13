// SPDX-License-Identifier: Apache-2.0
/* Management behavior shared by the Zephyr and TI BLE backends. */
#include "host_ble_session.h"
#include "host_protocol.h"
#include "application.h"

#include <errno.h>
#include <string.h>
#include <pbio/protocol.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(host_ble_session);

static struct host_ble_link *owner;
static struct k_spinlock session_lock;
static enum host_ble_status profile = HOST_BLE_CONNECTED;
static bool exit_compatibility;
static K_MUTEX_DEFINE(pybricks_lock);
static struct host_protocol pybricks;
/* Only bytes produced by host_protocol_event() are sent. Keep payload storage
 * outside BSS to avoid displacing the next MPU-aligned application stack. */
static __noinit uint8_t pybricks_pending[HOST_PROTOCOL_PAYLOAD_MAX];
static size_t pybricks_pending_size;
static int64_t pybricks_tx_deadline;
static void pybricks_output(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(pybricks_output_work, pybricks_output);
static uint32_t shell_session;
static int64_t shell_tx_deadline;
static bool shell_retired;

enum host_ble_status host_ble_session_profile(void)
{
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	enum host_ble_status status = owner ? profile : HOST_BLE_WAITING;
	k_spin_unlock(&session_lock, key);
	return status;
}

static void observe_pybricks(struct host_ble_link *conn)
{
	k_mutex_lock(&pybricks_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	if (conn == owner && !shell_session) {
		profile = HOST_BLE_PYBRICKS;
		if (IS_ENABLED(CONFIG_BRICKPHYR_PYBRICKS_EXECUTION)) {
			application_output_stream(pybricks.subscribed);
			application_message_attach(pybricks.subscribed ? host_ble_link_mtu(conn) - 4 : 0);
		}
	}
	k_spin_unlock(&session_lock, key);
	k_mutex_unlock(&pybricks_lock);
	k_work_schedule(&pybricks_output_work, K_NO_WAIT);
}

ssize_t host_ble_session_read(struct host_ble_link *conn, bool caps, uint16_t id,
			     void *buf, uint16_t len, uint16_t offset)
{
	if (conn != owner) return -BT_ATT_ERR_AUTHORIZATION;
	uint8_t value[HOST_PROTOCOL_PAYLOAD_MAX - 3];
	size_t length = host_protocol_read(caps ? PBIO_PYBRICKS_READ_SERVICE_PYBRICKS :
		PBIO_PYBRICKS_READ_SERVICE_GATT, id, host_hub_variant(),
		MIN(host_ble_link_mtu(conn) - 3, HOST_PROTOCOL_PAYLOAD_MAX - 1), value);
	if (offset > length) return -BT_ATT_ERR_INVALID_OFFSET;
	size_t count = MIN(len, length - offset);
	memcpy(buf, value + offset, count);
	if (caps) observe_pybricks(conn);
	return count;
}

ssize_t host_ble_session_command(struct host_ble_link *conn, const void *buf,
	uint16_t len, uint16_t offset, bool prepared)
{
	if (conn != owner || shell_session) {
		return -BT_ATT_ERR_AUTHORIZATION;
	}
	if (offset || prepared) {
		return -BT_ATT_ERR_NOT_SUPPORTED;
	}
	if (k_mutex_lock(&pybricks_lock, K_NO_WAIT)) {
		return -PBIO_PYBRICKS_ERROR_BUSY;
	}
	if (!host_ble_request_begin()) {
		k_mutex_unlock(&pybricks_lock);
		return -PBIO_PYBRICKS_ERROR_BUSY;
	}
	uint8_t error = host_protocol_command(&pybricks, buf, len);
	host_ble_request_end();
	if (error) {
		k_mutex_unlock(&pybricks_lock);
		return -error;
	}
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	exit_compatibility = pybricks.exit_compatibility;
	profile = HOST_BLE_PYBRICKS;
	if (IS_ENABLED(CONFIG_BRICKPHYR_PYBRICKS_EXECUTION)) {
		application_output_stream(pybricks.subscribed);
		application_message_attach(pybricks.subscribed ? host_ble_link_mtu(conn) - 4 : 0);
	}
	k_spin_unlock(&session_lock, key);
	k_mutex_unlock(&pybricks_lock);
	k_work_schedule(&pybricks_output_work, K_NO_WAIT);
	return len;
}

ssize_t host_ble_session_ccc_check(struct host_ble_link *conn, uint16_t value)
{
	if (conn != owner) {
		return -BT_ATT_ERR_AUTHORIZATION;
	}
	if (value > 1) {
		return -BT_ATT_ERR_VALUE_NOT_ALLOWED;
	}
	if (value) {
		observe_pybricks(conn);
	}
	return sizeof(value);
}

void host_ble_session_subscribed(uint16_t value)
{
	k_mutex_lock(&pybricks_lock, K_FOREVER);
	pybricks.subscribed = value == 1 && owner;
	pybricks.status_dirty = true;
	pybricks_pending_size = 0;
	pybricks_tx_deadline = 0;
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	if (IS_ENABLED(CONFIG_BRICKPHYR_PYBRICKS_EXECUTION)) {
		application_output_stream(pybricks.subscribed && !shell_session &&
			profile == HOST_BLE_PYBRICKS);
		application_message_attach(pybricks.subscribed && !shell_session &&
			profile == HOST_BLE_PYBRICKS ? host_ble_link_mtu(owner) - 4 : 0);
	}
	pybricks.message_request = 0;
	k_spin_unlock(&session_lock, key);
	k_mutex_unlock(&pybricks_lock);
	k_work_reschedule(&pybricks_output_work, K_NO_WAIT);
}

static void pybricks_output(struct k_work *work)
{
	if (k_mutex_lock(&pybricks_lock, K_NO_WAIT)) {
		k_work_reschedule(&pybricks_output_work, K_MSEC(20));
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	struct host_ble_link *conn = owner && !shell_session && profile == HOST_BLE_PYBRICKS &&
		pybricks.subscribed ? host_ble_link_ref(owner) : NULL;
	k_spin_unlock(&session_lock, key);
	if (!conn) {
		k_mutex_unlock(&pybricks_lock);
		return;
	}
	if (IS_ENABLED(CONFIG_BRICKPHYR_PYBRICKS_EXECUTION)) {
		application_message_attach(host_ble_link_mtu(conn) - 4);
	}
	if (!host_protocol_event_valid(&pybricks)) {
		pybricks_pending_size = 0;
		pybricks.message_request = 0;
	}
	if (!pybricks_pending_size) {
		pybricks_pending_size = host_protocol_event(&pybricks, true, k_uptime_get_32(), pybricks_pending,
			MIN(sizeof(pybricks_pending), host_ble_link_mtu(conn) - 3));
	}
	bool pending = pybricks_pending_size != 0;
	int err = pending ? host_ble_link_notify(conn, HOST_BLE_VALUE_PYBRICKS,
		pybricks_pending, pybricks_pending_size) : 0;
	if (!err) {
		if (pending) host_protocol_event_sent(&pybricks);
		pybricks_pending_size = 0;
		pybricks_tx_deadline = 0;
	} else if (!pybricks_tx_deadline) {
		pybricks_tx_deadline = k_uptime_get() + 3000;
	}
	bool retry = !err || ((err == -ENOMEM || err == -EAGAIN) &&
		k_uptime_get() < pybricks_tx_deadline);
	if (retry) {
		/* Drain available output one notification per work item. Only idle
		 * status polling and allocation backpressure need the delayed retry. */
		k_work_reschedule(&pybricks_output_work, pending && !err ? K_NO_WAIT :
			K_MSEC(IS_ENABLED(CONFIG_BRICKPHYR_PYBRICKS_EXECUTION) || pybricks_pending_size ?
				20 : HOST_PROTOCOL_STATUS_INTERVAL_MS));
	}
	k_mutex_unlock(&pybricks_lock);
	if (!retry) {
		LOG_ERR("Pybricks output failed: %d; closing link", err);
		host_ble_link_disconnect(conn);
	}
	host_ble_link_unref(conn);
}

static void shell_output(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(shell_output_work, shell_output);

static void shell_tx_ready(void)
{
	/* Do not move a pending retry: continuous writers cannot starve output. */
	k_work_schedule(&shell_output_work, K_NO_WAIT);
}

ssize_t host_ble_session_shell_subscribe(struct host_ble_link *conn, uint16_t value)
{
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	int err = 0;
	if (conn != owner || value > 1) {
		err = BT_ATT_ERR_AUTHORIZATION;
	} else if (value && shell_retired) {
		/* A new link excludes notifications already queued for the old terminal. */
		err = BT_ATT_ERR_AUTHORIZATION;
	} else if (value && !shell_session) {
		uint32_t session;
		if (host_ble_shell_open(shell_tx_ready, &session)) {
			err = BT_ATT_ERR_INSUFFICIENT_RESOURCES;
		} else {
			shell_session = session;
			shell_tx_deadline = 0;
			profile = HOST_BLE_NATIVE;
			application_output_stream(false);
			application_message_attach(0);
		}
	} else if (!value && shell_session) {
		host_ble_shell_close(shell_session);
		shell_session = 0;
		shell_retired = true;
	}
	k_spin_unlock(&session_lock, key);
	return err ? -err : sizeof(value);
}

ssize_t host_ble_session_shell_input(struct host_ble_link *conn, const void *buf,
	uint16_t len, uint16_t offset, bool prepared)
{
	if (offset || prepared) {
		return -BT_ATT_ERR_NOT_SUPPORTED;
	}
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	int err = conn == owner && shell_session ?
		host_ble_shell_receive(shell_session, buf, len) : -ENOTCONN;
	k_spin_unlock(&session_lock, key);
	return err ? -(err == -ENOBUFS ? BT_ATT_ERR_INSUFFICIENT_RESOURCES :
		BT_ATT_ERR_AUTHORIZATION) : len;
}

static void shell_output(struct k_work *work)
{
	uint8_t data[HOST_PROTOCOL_PAYLOAD_MAX];
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	struct host_ble_link *conn = shell_session && owner ? host_ble_link_ref(owner) : NULL;
	uint32_t session = shell_session;
	k_spin_unlock(&session_lock, key);
	if (!conn) {
		return;
	}
	/* Respect the peer's MTU, including before exchange has completed. */
	size_t capacity = MIN(sizeof(data), host_ble_link_mtu(conn) - 3);
	size_t length = host_ble_shell_peek(session, data, capacity);
	if (!length) {
		key = k_spin_lock(&session_lock);
		if (session == shell_session) {
			shell_tx_deadline = 0;
		}
		k_spin_unlock(&session_lock, key);
		host_ble_link_unref(conn);
		return;
	}
	/* The backend accepts notifications without blocking the system workqueue.
	 * Retry temporary backpressure for at most 3 s; never lose a byte silently.
	 * The deadline bounds a stalled diagnostic link, not its throughput.
	 */
	int err = host_ble_link_notify(conn, HOST_BLE_VALUE_SHELL, data, length);
	key = k_spin_lock(&session_lock);
	bool current = session == shell_session && owner == conn;
	if (current && !err) {
		host_ble_shell_consume(session, length);
		shell_tx_deadline = 0;
	}
	if (current && err && !shell_tx_deadline) {
		shell_tx_deadline = k_uptime_get() + 3000;
	}
	bool retry = current && (!err || ((err == -ENOMEM || err == -EAGAIN) &&
		k_uptime_get() < shell_tx_deadline));
	if (retry) {
		k_work_reschedule(&shell_output_work, err ? K_MSEC(10) : K_NO_WAIT);
	} else if (current) {
		host_ble_shell_close(session);
		shell_session = 0;
		shell_retired = true;
	}
	k_spin_unlock(&session_lock, key);
	if (current && !retry) {
		LOG_ERR("BLE terminal output failed: %d; closing link", err);
		host_ble_link_disconnect(conn);
	}
	host_ble_link_unref(conn);
}

int host_ble_session_open(struct host_ble_link *conn)
{
	int err = host_ble_acquire();
	if (err) return err;
	k_mutex_lock(&pybricks_lock, K_FOREVER);
	host_protocol_reset(&pybricks);
	pybricks_pending_size = 0;
	pybricks_tx_deadline = 0;
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	owner = host_ble_link_ref(conn);
	profile = HOST_BLE_CONNECTED;
	exit_compatibility = false;
	shell_retired = false;
	k_spin_unlock(&session_lock, key);
	k_mutex_unlock(&pybricks_lock);
	return 0;
}

void host_ble_session_close(struct host_ble_link *conn)
{
	k_mutex_lock(&pybricks_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	bool owned = conn == owner;
	if (owned) {
		owner = NULL;
		application_message_attach(0);
		host_protocol_reset(&pybricks);
		pybricks_pending_size = 0;
		k_work_cancel_delayable(&pybricks_output_work);
		shell_session = 0;
		k_work_cancel_delayable(&shell_output_work);
		/* Keep the shared owner/power hold until any accepted packet finishes.
		 * Do not block RX: the SMP sender may need its disconnect callback. */
		host_ble_release(exit_compatibility);
		exit_compatibility = false;
	}
	k_spin_unlock(&session_lock, key);
	k_mutex_unlock(&pybricks_lock);
	if (owned) {
		host_ble_link_unref(conn);
	}
}

bool host_ble_session_smp_begin(struct host_ble_link *conn)
{
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	bool valid = owner == conn && host_ble_request_begin();
	if (valid && profile != HOST_BLE_NATIVE) {
		profile = HOST_BLE_NATIVE;
		if (IS_ENABLED(CONFIG_BRICKPHYR_PYBRICKS_EXECUTION)) {
			application_output_stream(false);
			application_message_attach(0);
		}
	}
	k_spin_unlock(&session_lock, key);
	return valid;
}

void host_ble_session_smp_end(void)
{
	host_ble_request_end();
}
