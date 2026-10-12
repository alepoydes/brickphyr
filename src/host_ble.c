// SPDX-License-Identifier: Apache-2.0
#include "host_protocol.h"
#include "host_transport.h"
#include "application.h"
#include "hub.h"
#include "hub_radio.h"

#include <string.h>
#include <pbio/protocol.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(host_ble);
BUILD_ASSERT(CONFIG_BT_MAX_CONN == 1, "One management connection");

static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(0);
static struct bt_uuid_128 command_uuid = BT_UUID_INIT_128(0);
static struct bt_uuid_128 capabilities_uuid = BT_UUID_INIT_128(0);
/* Connection and GATT callbacks execute on the Bluetooth RX workqueue. */
static struct bt_conn *owner;
static struct bt_conn *disconnect_pending;
static struct k_spinlock session_lock;
static enum host_ble_status profile = HOST_BLE_CONNECTED;
static void request_disconnect(struct bt_conn *conn);
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
static K_MUTEX_DEFINE(radio_lock);
enum radio_state { RADIO_OFF, RADIO_STARTING, RADIO_ON, RADIO_STOPPING, RADIO_FAILED };
static atomic_t radio_state;
static atomic_t shutting_down;
static int radio_error;
static const char *failed_operation;
static struct k_work_q radio_queue;
/* F4 startup/re-advertising measured 548 bytes; 1 KiB leaves 960 usable with MPU. */
static K_THREAD_STACK_DEFINE(radio_stack, 1024);

#ifdef CONFIG_BRICKPHYR_RADIO
/* Observation/filter equations are adapted from Pybricks pb_type_ble_radio.c
 * at a7b1b04ae (MIT, Copyright (c) 2023 The Pybricks Authors).
 * The shared radio worker owns scan commands; RX callbacks only copy samples. */
struct observed_channel {
	struct bp_radio_sample sample;
	uint8_t channel;
};
static struct k_spinlock observe_lock;
static struct observed_channel *observed;
static size_t observed_count;
static uint32_t observe_handle, observe_sequence;
static int observe_error;
static bool scanning;
static void update_scan(struct k_work *work);
static K_WORK_DEFINE(scan_work, update_scan);
static int radio_failed(const char *operation, int error);

static void receive_broadcast(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
	struct net_buf_simple *buffer)
{
	/* Accept every legacy advertising type, as the reference does for City
	 * firmware. Walk AD elements instead of assuming manufacturer data is first. */
	const uint8_t *data = buffer->data;
	size_t size = buffer->len;
	uint32_t now = k_uptime_get_32();
	while (size) {
		size_t length = data[0];
		if (!length || length >= size) return;
		if (length >= 4 && length <= 4 + BP_RADIO_DATA_MAX && data[1] == BT_DATA_MANUFACTURER_DATA &&
		    sys_get_le16(data + 2) == 0x0397 && rssi != BT_GAP_RSSI_INVALID && rssi != INT8_MIN) {
			k_spinlock_key_t key = k_spin_lock(&observe_lock);
			for (size_t i = 0; i < observed_count; i++) {
				if (observed[i].channel != data[4]) continue;
				struct bp_radio_sample *sample = &observed[i].sample;
				uint32_t elapsed = sample->valid ? MIN(now - sample->timestamp_ms, 512U) : 512U;
				sample->rssi = (sample->rssi * (512 - (int)elapsed) + rssi * (int)elapsed) / 512;
				sample->timestamp_ms = now;
				sample->size = length - 4;
				sample->valid = 1;
				memcpy(sample->data, data + 5, sample->size);
				break;
			}
			k_spin_unlock(&observe_lock, key);
		}
		size -= length + 1;
		data += length + 1;
	}
}

static void update_scan(struct k_work *work)
{
	k_mutex_lock(&radio_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&observe_lock);
	bool desired = observe_handle && observed_count && !atomic_get(&shutting_down);
	uint32_t handle = observe_handle;
	k_spin_unlock(&observe_lock, key);
	int state = atomic_get(&radio_state);
	int err = atomic_get(&shutting_down) ? -ESHUTDOWN :
		state == RADIO_FAILED ? (radio_error < 0 ? radio_error : -EIO) :
		state != RADIO_ON ? -EHOSTDOWN : 0;
	if (!err && desired != scanning) {
		/* Reference passive scan: 70 ms interval / 35 ms window, leaving
		 * radio time for existing connections and advertising. No duplicate
		 * filter: repeated values still refresh their age and signal strength. */
		err = desired ? bt_le_scan_start(BT_LE_SCAN_PARAM(BT_LE_SCAN_TYPE_PASSIVE,
			0, 0x70, 0x38), receive_broadcast) : bt_le_scan_stop();
		if (err) radio_failed(desired ? "scan start" : "scan stop", err);
		else scanning = desired;
	}
	key = k_spin_lock(&observe_lock);
	if (handle == observe_handle) observe_error = err > 0 ? -EIO : err;
	k_spin_unlock(&observe_lock, key);
	k_mutex_unlock(&radio_lock);
}

int hub_radio_observe(const uint8_t *channels, size_t count, uint32_t *handle)
{
	if (count > BP_RADIO_CHANNELS_MAX) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&observe_lock);
	int state = atomic_get(&radio_state);
	int err = observe_handle ? -EBUSY : atomic_get(&shutting_down) ? -ESHUTDOWN :
		state == RADIO_FAILED ? (radio_error < 0 ? radio_error : -EIO) :
		state != RADIO_ON ? -EHOSTDOWN : 0;
	if (!err) {
		/* Bounded, nonblocking allocation/publication under the same lock:
		 * aborting the caller cannot leak an unpublished system-heap buffer. */
		struct observed_channel *copy = count ? k_calloc(count, sizeof(*copy)) : NULL;
		if (count && !copy) err = -ENOMEM;
		else {
			for (size_t i = 0; i < count; i++) {
				copy[i].channel = channels[i];
				copy[i].sample.rssi = INT8_MIN;
			}
			observed = copy;
			observed_count = count;
			observe_sequence++;
			if (!observe_sequence) observe_sequence++;
			*handle = observe_handle = observe_sequence;
			observe_error = -EAGAIN;
			k_work_submit_to_queue(&radio_queue, &scan_work);
		}
	}
	k_spin_unlock(&observe_lock, key);
	return err;
}

int hub_radio_ready(uint32_t handle)
{
	k_spinlock_key_t key = k_spin_lock(&observe_lock);
	int err = !handle || handle != observe_handle ? -ESTALE :
		atomic_get(&shutting_down) ? -ESHUTDOWN : observe_error;
	if (!err && atomic_get(&radio_state) != RADIO_ON) err = -EHOSTDOWN;
	k_spin_unlock(&observe_lock, key);
	return err;
}

int hub_radio_get(uint32_t handle, uint32_t channel, struct bp_radio_sample *sample)
{
	k_spinlock_key_t key = k_spin_lock(&observe_lock);
	int err = !handle || handle != observe_handle ? -ESTALE :
		atomic_get(&shutting_down) ? -ESHUTDOWN : observe_error;
	if (!err && atomic_get(&radio_state) != RADIO_ON) err = -EHOSTDOWN;
	if (!err) {
		err = -EINVAL;
		for (size_t i = 0; i < observed_count; i++) {
			if (observed[i].channel != channel) continue;
			*sample = observed[i].sample;
			if (k_uptime_get_32() - sample->timestamp_ms > 1000U) {
				sample->valid = sample->size = 0;
				sample->rssi = INT8_MIN;
			}
			err = 0;
			break;
		}
	}
	k_spin_unlock(&observe_lock, key);
	return err;
}

static int retire_observer(uint32_t handle, bool force)
{
	k_spinlock_key_t key = k_spin_lock(&observe_lock);
	if (observe_handle && (force || handle == observe_handle)) {
		/* No driver retains this buffer. Detach under the RX lock, before
		 * asynchronous scan shutdown, so forced application stop never waits
		 * for an HCI reply or leaves a callback pointing into released memory. */
		k_free(observed);
		observed = NULL;
		observed_count = observe_handle = 0;
		k_work_submit_to_queue(&radio_queue, &scan_work);
	}
	k_spin_unlock(&observe_lock, key);
	return 0;
}

int hub_radio_close(uint32_t handle) { return retire_observer(handle, false); }
int hub_radio_release(void) { return retire_observer(0, true); }
#endif

enum host_ble_status host_ble_status(void)
{
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	enum host_ble_status status = owner ? profile : HOST_BLE_WAITING;
	if (atomic_get(&radio_state) != RADIO_ON || atomic_get(&shutting_down)) {
		status = HOST_BLE_OFF;
	}
	k_spin_unlock(&session_lock, key);
	return status;
}

static void observe_pybricks(struct bt_conn *conn)
{
	k_mutex_lock(&pybricks_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	if (conn == owner && !shell_session) {
		profile = HOST_BLE_PYBRICKS;
		application_output_stream(pybricks.subscribed);
		application_message_attach(pybricks.subscribed ? bt_gatt_get_mtu(conn) - 4 : 0);
	}
	k_spin_unlock(&session_lock, key);
	k_mutex_unlock(&pybricks_lock);
	k_work_schedule(&pybricks_output_work, K_NO_WAIT);
}

static ssize_t read_value(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	if (conn != owner) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	bool caps = attr->uuid == &capabilities_uuid.uuid;
	uint16_t id = caps ? 3 : BT_UUID_16(attr->uuid)->val;
	uint8_t value[HOST_PROTOCOL_PAYLOAD_MAX - 3];
	size_t length = host_protocol_read(caps ? PBIO_PYBRICKS_READ_SERVICE_PYBRICKS :
		PBIO_PYBRICKS_READ_SERVICE_GATT, id, host_hub_variant(),
		MIN(bt_gatt_get_mtu(conn) - 3, HOST_PROTOCOL_PAYLOAD_MAX - 1), value);
	ssize_t result = bt_gatt_attr_read(conn, attr, buf, len, offset, value, length);
	if (caps && result >= 0) {
		observe_pybricks(conn);
	}
	return result;
}

static ssize_t write_command(struct bt_conn *conn, const struct bt_gatt_attr *attr,
	const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (conn != owner || shell_session) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (offset || (flags & BT_GATT_WRITE_FLAG_PREPARE)) {
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}
	if (k_mutex_lock(&pybricks_lock, K_NO_WAIT)) {
		return BT_GATT_ERR(PBIO_PYBRICKS_ERROR_BUSY);
	}
	if (!host_ble_request_begin()) {
		k_mutex_unlock(&pybricks_lock);
		return BT_GATT_ERR(PBIO_PYBRICKS_ERROR_BUSY);
	}
	uint8_t error = host_protocol_command(&pybricks, buf, len);
	host_ble_request_end();
	if (error) {
		k_mutex_unlock(&pybricks_lock);
		return BT_GATT_ERR(error);
	}
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	exit_compatibility = pybricks.exit_compatibility;
	profile = HOST_BLE_PYBRICKS;
	application_output_stream(pybricks.subscribed);
	application_message_attach(pybricks.subscribed ? bt_gatt_get_mtu(conn) - 4 : 0);
	k_spin_unlock(&session_lock, key);
	k_mutex_unlock(&pybricks_lock);
	k_work_schedule(&pybricks_output_work, K_NO_WAIT);
	return len;
}

static ssize_t reject_stdin(struct bt_conn *conn, const struct bt_gatt_attr *attr,
	const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	return BT_GATT_ERR(PBIO_PYBRICKS_ERROR_INVALID_COMMAND);
}

static ssize_t allow_subscription(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				  uint16_t value)
{
	if (conn != owner) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (value > BT_GATT_CCC_NOTIFY) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	if (value) {
		observe_pybricks(conn);
	}
	return sizeof(value);
}

static void subscribed(const struct bt_gatt_attr *attr, uint16_t value)
{
	k_mutex_lock(&pybricks_lock, K_FOREVER);
	pybricks.subscribed = value == BT_GATT_CCC_NOTIFY && owner;
	pybricks.status_dirty = true;
	pybricks_pending_size = 0;
	pybricks_tx_deadline = 0;
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	application_output_stream(pybricks.subscribed && !shell_session &&
		profile == HOST_BLE_PYBRICKS);
	application_message_attach(pybricks.subscribed && !shell_session &&
		profile == HOST_BLE_PYBRICKS ? bt_gatt_get_mtu(owner) - 4 : 0);
	pybricks.message_request = 0;
	k_spin_unlock(&session_lock, key);
	k_mutex_unlock(&pybricks_lock);
	k_work_reschedule(&pybricks_output_work, K_NO_WAIT);
}

BT_GATT_SERVICE_DEFINE(pybricks_service,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),
	BT_GATT_CHARACTERISTIC(&command_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_WRITE, NULL, write_command, NULL),
	BT_GATT_CCC_WITH_WRITE_CB(subscribed, allow_subscription,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&capabilities_uuid.uuid, BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ, read_value, NULL, NULL));

static void pybricks_output(struct k_work *work)
{
	if (k_mutex_lock(&pybricks_lock, K_NO_WAIT)) {
		k_work_reschedule(&pybricks_output_work, K_MSEC(20));
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	struct bt_conn *conn = owner && !shell_session && profile == HOST_BLE_PYBRICKS &&
		pybricks.subscribed ? bt_conn_ref(owner) : NULL;
	k_spin_unlock(&session_lock, key);
	if (!conn) {
		k_mutex_unlock(&pybricks_lock);
		return;
	}
	application_message_attach(bt_gatt_get_mtu(conn) - 4);
	if (!host_protocol_event_valid(&pybricks)) {
		pybricks_pending_size = 0;
		pybricks.message_request = 0;
	}
	if (!pybricks_pending_size) {
		pybricks_pending_size = host_protocol_event(&pybricks, true, k_uptime_get_32(), pybricks_pending,
			MIN(sizeof(pybricks_pending), bt_gatt_get_mtu(conn) - 3));
	}
	bool pending = pybricks_pending_size != 0;
	int err = pending ? bt_gatt_notify(conn, &pybricks_service.attrs[2],
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
			K_MSEC(IS_ENABLED(CONFIG_BRICKPHYR_PYTHON) || pybricks_pending_size ?
				20 : HOST_PROTOCOL_STATUS_INTERVAL_MS));
	}
	k_mutex_unlock(&pybricks_lock);
	if (!retry) {
		LOG_ERR("Pybricks output failed: %d; closing link", err);
		request_disconnect(conn);
	}
	bt_conn_unref(conn);
}

BT_GATT_SERVICE_DEFINE(device_information,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DIS),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_FIRMWARE_REVISION, BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ, read_value, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_SOFTWARE_REVISION, BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ, read_value, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_PNP_ID, BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ, read_value, NULL, NULL));

/* Keep legacy NUS discovery/subscription for existing clients. Program I/O uses
 * the Pybricks command/event characteristic; legacy stdin is unsupported. */
BT_GATT_SERVICE_DEFINE(legacy_uart,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(BT_UUID_NUS_SRV_VAL)),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BT_UUID_NUS_TX_CHAR_VAL),
		BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC_WITH_WRITE_CB(NULL, allow_subscription,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BT_UUID_NUS_RX_CHAR_VAL),
		BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, reject_stdin, NULL));

/* Native interface's terminal stream. Distinct UUIDs keep Pybricks stdout and
 * its legacy NUS subscriptions separate; payloads are ordinary Shell bytes. */
#define SHELL_UUID(id) BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xf3641400 + (id), \
	0x00b0, 0x4240, 0xba50, 0x05ca45bf8abc))

static void shell_output(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(shell_output_work, shell_output);

static void shell_tx_ready(void)
{
	/* Do not move a pending retry: continuous writers cannot starve output. */
	k_work_schedule(&shell_output_work, K_NO_WAIT);
}

static ssize_t shell_subscription(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				  uint16_t value)
{
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	int err = 0;
	if (conn != owner || value > BT_GATT_CCC_NOTIFY) {
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
	return err ? BT_GATT_ERR(err) : sizeof(value);
}

static ssize_t shell_input(struct bt_conn *conn, const struct bt_gatt_attr *attr,
	const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset || flags & BT_GATT_WRITE_FLAG_PREPARE) {
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	int err = conn == owner && shell_session ?
		host_ble_shell_receive(shell_session, buf, len) : -ENOTCONN;
	k_spin_unlock(&session_lock, key);
	return err ? BT_GATT_ERR(err == -ENOBUFS ? BT_ATT_ERR_INSUFFICIENT_RESOURCES :
		BT_ATT_ERR_AUTHORIZATION) : len;
}

BT_GATT_SERVICE_DEFINE(brickphyr_terminal,
	BT_GATT_PRIMARY_SERVICE(SHELL_UUID(0)),
	BT_GATT_CHARACTERISTIC(SHELL_UUID(1), BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC_WITH_WRITE_CB(NULL, shell_subscription,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(SHELL_UUID(2), BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE, NULL, shell_input, NULL));

static void shell_output(struct k_work *work)
{
	uint8_t data[CONFIG_BT_L2CAP_TX_MTU - 3];
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	struct bt_conn *conn = shell_session && owner ? bt_conn_ref(owner) : NULL;
	uint32_t session = shell_session;
	k_spin_unlock(&session_lock, key);
	if (!conn) {
		return;
	}
	/* Respect the peer's MTU, including before exchange has completed. */
	size_t capacity = MIN(sizeof(data), bt_gatt_get_mtu(conn) - 3);
	size_t length = host_ble_shell_peek(session, data, capacity);
	if (!length) {
		key = k_spin_lock(&session_lock);
		if (session == shell_session) {
			shell_tx_deadline = 0;
		}
		k_spin_unlock(&session_lock, key);
		bt_conn_unref(conn);
		return;
	}
	/* Zephyr notify on the system workqueue uses nonblocking allocation.
	 * Retry temporary backpressure for at most 3 s; never lose a byte silently.
	 * The deadline bounds a stalled diagnostic link, not its throughput.
	 */
	int err = bt_gatt_is_subscribed(conn, &brickphyr_terminal.attrs[2], BT_GATT_CCC_NOTIFY) ?
		bt_gatt_notify(conn, &brickphyr_terminal.attrs[2], data, length) : -EAGAIN;
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
	}
	k_spin_unlock(&session_lock, key);
	if (current && !retry) {
		LOG_ERR("BLE terminal output failed: %d; closing link", err);
		request_disconnect(conn);
	}
	bt_conn_unref(conn);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		return;
	}
	if (atomic_get(&radio_state) == RADIO_FAILED || host_ble_acquire()) {
		request_disconnect(conn);
		return;
	}
	k_mutex_lock(&pybricks_lock, K_FOREVER);
	host_protocol_reset(&pybricks);
	pybricks_pending_size = 0;
	pybricks_tx_deadline = 0;
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	owner = bt_conn_ref(conn);
	profile = HOST_BLE_CONNECTED;
	exit_compatibility = false;
	shell_retired = false;
	k_spin_unlock(&session_lock, key);
	k_mutex_unlock(&pybricks_lock);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
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
		bt_conn_unref(conn);
	}
}

#if defined(CONFIG_MCUMGR_TRANSPORT_BT)
static bool request_begin(struct bt_conn *conn)
{
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	bool valid = owner == conn && host_ble_request_begin();
	if (valid && profile != HOST_BLE_NATIVE) {
		profile = HOST_BLE_NATIVE;
		application_output_stream(false);
		application_message_attach(0);
	}
	k_spin_unlock(&session_lock, key);
	return valid;
}

static void request_end(struct bt_conn *conn, int result)
{
	host_ble_request_end();
}

static const struct smp_bt_request_callbacks request_callbacks = {
	.begin = request_begin, .end = request_end,
};
#endif

static int advertise(void)
{
#if !defined(CONFIG_MCUMGR_TRANSPORT_BT)
	uint8_t pnp[2 + PBIO_PYBRICKS_PNP_ID_SIZE];
	sys_put_le16(PBIO_GATT_PNP_ID_CHAR_UUID, pnp);
	pbio_pybricks_pnp_id(pnp + 2, PBIO_PYBRICKS_HUB_KIND_PRIME, host_hub_variant());
#endif
	const struct bt_data data[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
		BT_DATA(BT_DATA_UUID128_SOME, service_uuid.val, sizeof(service_uuid.val)),
	};
	const struct bt_data response[] = {
#if defined(CONFIG_MCUMGR_TRANSPORT_BT)
		/* Legacy advertising has 31 bytes per packet. Put the second UUID
		 * beside the name in scan response; PnP remains readable over GATT. */
		BT_DATA_BYTES(BT_DATA_UUID128_SOME, SMP_BT_SVC_UUID_VAL),
#else
		BT_DATA(BT_DATA_SVC_DATA16, pnp, sizeof(pnp)),
#endif
		BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	};
	return bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, data, ARRAY_SIZE(data),
		response, ARRAY_SIZE(response));
}

/* Serialized by radio_lock after setup. Keep the first failure until reboot:
 * a failed HCI operation does not establish that the controller is stopped or reset. */
static int radio_failed(const char *operation, int error)
{
	if (atomic_get(&radio_state) == RADIO_FAILED) {
		return radio_error;
	}
	radio_error = error;
	failed_operation = operation;
	atomic_set(&radio_state, RADIO_FAILED);
	LOG_ERR("Bluetooth %s failed: %d; reboot required", operation, error);
	return error;
}

static void disconnect_peer(struct k_work *work)
{
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	struct bt_conn *conn = disconnect_pending;
	k_spin_unlock(&session_lock, key);

	k_mutex_lock(&radio_lock, K_FOREVER);
	int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if (err && err != -ENOTCONN) {
		radio_failed("disconnect", err);
	}
	k_mutex_unlock(&radio_lock);

	key = k_spin_lock(&session_lock);
	disconnect_pending = NULL;
	k_spin_unlock(&session_lock, key);
	bt_conn_unref(conn);
}
static K_WORK_DEFINE(disconnect_work, disconnect_peer);

static void request_disconnect(struct bt_conn *conn)
{
	/* One connection slot, retained until the worker finishes. HCI waits must
	 * not block either the system/button queue or Bluetooth RX callbacks.
	 * Only the real disconnection callback releases management ownership. */
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	if (!disconnect_pending) {
		disconnect_pending = bt_conn_ref(conn);
		k_work_submit_to_queue(&radio_queue, &disconnect_work);
	}
	k_spin_unlock(&session_lock, key);
}

static int set_radio(bool enable)
{
	k_mutex_lock(&radio_lock, K_FOREVER);
	if (enable && atomic_get(&shutting_down)) {
		k_mutex_unlock(&radio_lock);
		return -ESHUTDOWN;
	}
	int state = atomic_get(&radio_state);
	int err = state == RADIO_FAILED ? radio_error : 0;
	if (!err && state != (enable ? RADIO_ON : RADIO_OFF)) {
		atomic_set(&radio_state, enable ? RADIO_STARTING : RADIO_STOPPING);
		const char *operation = enable ? "initialization" : "advertising stop";
#ifdef CONFIG_BRICKPHYR_RADIO
		if (!enable && scanning) {
			operation = "scan stop";
			err = bt_le_scan_stop();
			if (!err) scanning = false;
		}
#endif
		if (!err) {
			operation = enable ? "initialization" : "advertising stop";
			err = enable ? bt_enable(NULL) : bt_le_adv_stop();
		}
		if (!err) {
			operation = enable ? "advertising start" : "controller shutdown";
			err = enable ? (atomic_get(&shutting_down) ? 0 : advertise()) : bt_disable();
		}
		if (err) {
			radio_failed(operation, err);
		} else {
			atomic_set(&radio_state, enable ? RADIO_ON : RADIO_OFF);
#ifdef CONFIG_BRICKPHYR_RADIO
			k_spinlock_key_t key = k_spin_lock(&observe_lock);
			if (!enable) {
				for (size_t i = 0; i < observed_count; i++) {
					observed[i].sample.valid = observed[i].sample.size = 0;
					observed[i].sample.rssi = INT8_MIN;
				}
			} else if (observe_handle) {
				observe_error = -EAGAIN;
				k_work_submit_to_queue(&radio_queue, &scan_work);
			}
			k_spin_unlock(&observe_lock, key);
#endif
		}
	}
	k_mutex_unlock(&radio_lock);
	return err;
}

static void start_radio(struct k_work *work)
{
	set_radio(true);
}
static K_WORK_DEFINE(startup_work, start_radio);

static void resume_advertising(struct k_work *work)
{
	k_mutex_lock(&radio_lock, K_FOREVER);
	if (!atomic_get(&shutting_down) && atomic_get(&radio_state) == RADIO_ON) {
		int err = advertise();
		if (err) {
			radio_failed("advertising resume", err);
		}
	}
	k_mutex_unlock(&radio_lock);
}
static K_WORK_DEFINE(advertising_work, resume_advertising);

static void stop_radio(struct k_work *work)
{
	set_radio(false);
	/* Serialized after any pending initialization; never wait for HCI on the
	 * hub thread that must still service the power button. */
	int err = hub_bluetooth_power_off();
	if (err) {
		LOG_ERR("Bluetooth power off failed: %d", err);
	}
}
static K_WORK_DEFINE(shutdown_work, stop_radio);

void host_ble_shutdown(void)
{
	if (!atomic_cas(&shutting_down, 0, 1)) {
		return;
	}
	k_work_cancel(&startup_work);
	k_work_cancel(&advertising_work);
	k_work_submit_to_queue(&radio_queue, &shutdown_work);
}

static void recycled(void)
{
	int state = atomic_get(&radio_state);
	if (state == RADIO_ON || state == RADIO_STARTING) {
		k_work_submit_to_queue(&radio_queue, &advertising_work);
	}
}

BT_CONN_CB_DEFINE(host_ble_callbacks) = {
	.connected = connected, .disconnected = disconnected, .recycled = recycled,
};

static int cmd_bluetooth(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		const char *const names[] = {"off", "starting", "on", "stopping", "failed"};
		int state = atomic_get(&radio_state);
		shell_print(shell, "Bluetooth: %s", names[state]);
		if (state == RADIO_FAILED) {
			shell_print(shell, "%s failed: %d; reboot required", failed_operation, radio_error);
		}
		return 0;
	}
	bool enable = !strcmp(argv[1], "on");
	if (!enable && strcmp(argv[1], "off")) {
		return -EINVAL;
	}
	/* An explicit choice supersedes a queued boot request. Join an already
	 * running startup before applying it; never let a late boot job undo off. */
	struct k_work_sync sync;
	k_work_cancel_sync(&startup_work, &sync);
	k_work_cancel_sync(&advertising_work, &sync);
	int err = set_radio(enable);
	if (err == -ESHUTDOWN) {
		shell_error(shell, "Hub is shutting down.");
	} else if (err) {
		shell_error(shell, "Bluetooth %s failed: %d; reboot required", failed_operation, err);
	} else {
		shell_print(shell, "%s", enable ?
			"Bluetooth on. Close USB before connecting over BLE." : "Bluetooth off.");
	}
	return err;
}
SHELL_CMD_ARG_REGISTER(bluetooth, NULL, "Bluetooth status, or on | off.", cmd_bluetooth, 1, 1);

static int setup(void)
{
	/* PBIO exports canonical UUID bytes; Zephyr stores their wire order. */
	sys_memcpy_swap(service_uuid.val, pbio_pybricks_service_uuid, sizeof(service_uuid.val));
	sys_memcpy_swap(command_uuid.val, pbio_pybricks_command_event_char_uuid,
		sizeof(command_uuid.val));
	sys_memcpy_swap(capabilities_uuid.val, pbio_pybricks_hub_capabilities_char_uuid,
		sizeof(capabilities_uuid.val));
	/* Keep shutdown available even when profile registration fails. Radio waits
	 * stay below USB/UI priority, outside the system device workqueue. */
	const struct k_work_queue_config config = {.name = "host_ble"};
	k_work_queue_start(&radio_queue, radio_stack, K_THREAD_STACK_SIZEOF(radio_stack),
		K_LOWEST_APPLICATION_THREAD_PRIO - IS_ENABLED(CONFIG_BRICKPHYR_NATIVE), &config);
#if defined(CONFIG_MCUMGR_TRANSPORT_BT)
	int err = smp_bt_register_request_callbacks(&request_callbacks);
	if (err) {
		return radio_failed("SMP callback registration", err);
	}
#endif
	k_work_submit_to_queue(&radio_queue, &startup_work);
	return 0;
}
SYS_INIT(setup, APPLICATION, 90);
