// SPDX-License-Identifier: Apache-2.0
#define __ZEPHYR_SUPERVISOR__
#include "host_protocol.h"
#include "host_ble_session.h"
#include "host_transport.h"
#include "application.h"
#include "hub.h"
#include "hub_radio.h"
#include "hub_ble_peer.h"
#include "hub_network.h"
#include "host_radio.h"

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

static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(0);
static struct bt_uuid_128 command_uuid = BT_UUID_INIT_128(0);
static struct bt_uuid_128 capabilities_uuid = BT_UUID_INIT_128(0);
/* Connection and GATT callbacks execute on the Bluetooth RX workqueue. */
static struct bt_conn *disconnect_pending[CONFIG_BT_MAX_CONN];
static struct k_spinlock session_lock;
static void request_disconnect(struct bt_conn *conn);
static K_MUTEX_DEFINE(radio_lock);
enum radio_state { RADIO_OFF, RADIO_STARTING, RADIO_ON, RADIO_STOPPING, RADIO_FAILED };
static atomic_t radio_state;
static atomic_t shutting_down;
/* Only incoming peripheral links consume the management advertisement.
 * Central links and their recycled connection objects do not. */
static atomic_t advertising;
static int radio_error;
static const char *failed_operation;
static struct k_work_q radio_queue;
static struct k_work startup_work;
/* F4 startup/re-advertising measured 548 bytes; inspected central call chains
 * reach 544 bytes. Reserve 1 KiB plus the guard; central high-water is pending. */
static K_KERNEL_STACK_DEFINE(radio_stack, 1024);

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
static int scanning; /* 0 stopped, 1 passive observation, 2 active discovery. */
static void update_scan(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(scan_work, update_scan);
static int radio_failed(const char *operation, int error);

void host_radio_schedule(k_timeout_t delay)
{
	if (K_TIMEOUT_EQ(delay, K_NO_WAIT)) k_work_reschedule_for_queue(&radio_queue, &scan_work, delay);
	else k_work_schedule_for_queue(&radio_queue, &scan_work, delay);
}

bool host_radio_starting(void)
{
	int state = atomic_get(&radio_state);
	return !atomic_get(&shutting_down) && (state == RADIO_STARTING ||
		(state == RADIO_OFF && k_work_busy_get(&startup_work)));
}

int host_radio_error(void)
{
	int state = atomic_get(&radio_state);
	return atomic_get(&shutting_down) ? -ESHUTDOWN :
		state == RADIO_FAILED ? (radio_error < 0 ? radio_error : -EIO) :
		state != RADIO_ON ? -EHOSTDOWN : 0;
}

static void receive_broadcast(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
	struct net_buf_simple *buffer)
{
#ifdef CONFIG_BRICKPHYR_BLE_PEER
	hub_ble_peer_receive(addr, type, buffer->data, buffer->len);
#endif
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
	int peer_mode = hub_ble_peer_scan_mode();
	int wanted = peer_mode == 1 ? 2 : peer_mode == 2 ? 0 : desired ? 1 : 0;
	int err = host_radio_error();
	if (!err && wanted != scanning) {
		if (scanning) {
			err = bt_le_scan_stop();
			if (err) radio_failed("scan stop", err);
			else scanning = 0;
		}
		/* Reference passive scan: 70 ms interval / 35 ms window, leaving
		 * radio time for existing connections and advertising. No duplicate
		 * filter: repeated values still refresh age/RSSI. Peripheral discovery temporarily
		 * uses the reference active 30/30 ms scan to obtain device names. */
		if (!err && wanted) {
			err = bt_le_scan_start(BT_LE_SCAN_PARAM(wanted == 2 ? BT_LE_SCAN_TYPE_ACTIVE :
				BT_LE_SCAN_TYPE_PASSIVE, 0, wanted == 2 ? 0x30 : 0x70,
				wanted == 2 ? 0x30 : 0x38), receive_broadcast);
			if (err) radio_failed("scan start", err);
			else scanning = wanted;
		}
	}
	/* Explicit scanning is stopped before CREATE. All GATT/HCI work shares
	 * this queue and lifecycle mutex; RX only publishes copied peer state. */
	hub_ble_peer_process(err, !scanning);
	hub_network_process(host_radio_error());
	if (hub_ble_peer_scan_mode() != peer_mode) host_radio_schedule(K_NO_WAIT);
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
			host_radio_schedule(K_NO_WAIT);
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

int hub_radio_version(char *value, size_t size)
{
	if (atomic_get(&shutting_down)) return -ESHUTDOWN;
	if (atomic_get(&radio_state) != RADIO_ON) return -EHOSTDOWN;
	/* The provider publishes immutable artifact metadata before RADIO_ON.
	 * No HCI request or system lock is needed from an application thread. */
	return hub_bluetooth_version(value, size);
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
		host_radio_schedule(K_NO_WAIT);
	}
	k_spin_unlock(&observe_lock, key);
	return 0;
}

int hub_radio_close(uint32_t handle) { return retire_observer(handle, false); }
int hub_radio_release(void) { return retire_observer(0, true); }
#endif

enum host_ble_status host_ble_status(void)
{
	return atomic_get(&radio_state) == RADIO_ON && !atomic_get(&shutting_down) ?
		host_ble_session_profile() : HOST_BLE_OFF;
}

struct host_ble_link *host_ble_link_ref(struct host_ble_link *link)
{
	return (struct host_ble_link *)bt_conn_ref((struct bt_conn *)link);
}

void host_ble_link_unref(struct host_ble_link *link)
{
	bt_conn_unref((struct bt_conn *)link);
}

uint16_t host_ble_link_mtu(struct host_ble_link *link)
{
	return bt_gatt_get_mtu((struct bt_conn *)link);
}

void host_ble_link_disconnect(struct host_ble_link *link)
{
	request_disconnect((struct bt_conn *)link);
}

static ssize_t read_value(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	bool caps = attr->uuid == &capabilities_uuid.uuid;
	return host_ble_session_read((struct host_ble_link *)conn, caps,
		caps ? 3 : BT_UUID_16(attr->uuid)->val, buf, len, offset);
}

static ssize_t write_command(struct bt_conn *conn, const struct bt_gatt_attr *attr,
	const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	return host_ble_session_command((struct host_ble_link *)conn, buf, len,
		offset, flags & BT_GATT_WRITE_FLAG_PREPARE);
}

static ssize_t reject_stdin(struct bt_conn *conn, const struct bt_gatt_attr *attr,
	const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	return BT_GATT_ERR(PBIO_PYBRICKS_ERROR_INVALID_COMMAND);
}

static ssize_t allow_subscription(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				 uint16_t value)
{
	return host_ble_session_ccc_check((struct host_ble_link *)conn, value);
}

static void subscribed(const struct bt_gatt_attr *attr, uint16_t value)
{
	host_ble_session_subscribed(value);
}

static ssize_t shell_subscription(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				 uint16_t value)
{
	return host_ble_session_shell_subscribe((struct host_ble_link *)conn, value);
}

static ssize_t shell_input(struct bt_conn *conn, const struct bt_gatt_attr *attr,
	const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	return host_ble_session_shell_input((struct host_ble_link *)conn, buf, len,
		offset, flags & BT_GATT_WRITE_FLAG_PREPARE);
}

#define SHELL_UUID(id) BT_UUID_DECLARE_128(HOST_BLE_SHELL_UUID(id))

BT_GATT_SERVICE_DEFINE(pybricks_service,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),
	BT_GATT_CHARACTERISTIC(&command_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_WRITE, NULL, write_command, NULL),
	BT_GATT_CCC_WITH_WRITE_CB(subscribed, allow_subscription,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&capabilities_uuid.uuid, BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ, read_value, NULL, NULL));

BT_GATT_SERVICE_DEFINE(device_information,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DIS),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_FIRMWARE_REVISION, BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ, read_value, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_SOFTWARE_REVISION, BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ, read_value, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_PNP_ID, BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ, read_value, NULL, NULL));

/* Current Pybricks clients subscribe to NUS, but program I/O uses the Pybricks
 * command/event characteristic; legacy stdin is unsupported. */
BT_GATT_SERVICE_DEFINE(legacy_uart,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(BT_UUID_NUS_SRV_VAL)),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BT_UUID_NUS_TX_CHAR_VAL),
		BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC_WITH_WRITE_CB(NULL, allow_subscription,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BT_UUID_NUS_RX_CHAR_VAL),
		BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, reject_stdin, NULL));

BT_GATT_SERVICE_DEFINE(brickphyr_terminal,
	BT_GATT_PRIMARY_SERVICE(SHELL_UUID(0)),
	BT_GATT_CHARACTERISTIC(SHELL_UUID(1), BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC_WITH_WRITE_CB(NULL, shell_subscription,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(SHELL_UUID(2), BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE, NULL, shell_input, NULL));

int host_ble_link_notify(struct host_ble_link *link, enum host_ble_value value,
			const uint8_t *data, size_t size)
{
	if (value != HOST_BLE_VALUE_PYBRICKS && value != HOST_BLE_VALUE_SHELL) return -EINVAL;
	struct bt_conn *conn = (struct bt_conn *)link;
	const struct bt_gatt_attr *attr = value == HOST_BLE_VALUE_PYBRICKS ?
		&pybricks_service.attrs[2] : &brickphyr_terminal.attrs[2];
	if (value == HOST_BLE_VALUE_SHELL &&
	    !bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY)) return -EAGAIN;
	return bt_gatt_notify(conn, attr, data, size);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;
	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE ||
	    info.role != BT_CONN_ROLE_PERIPHERAL) return;
	if (!err && (atomic_get(&radio_state) == RADIO_FAILED ||
	    host_ble_session_open((struct host_ble_link *)conn))) request_disconnect(conn);
	/* Keep the reservation until the common session publishes its owner.
	 * Serialize its release with advertise's owner check and reservation. */
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	atomic_clear(&advertising);
	k_spin_unlock(&session_lock, key);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	host_ble_session_close((struct host_ble_link *)conn);
}

#if defined(CONFIG_MCUMGR_TRANSPORT_BT)
static bool request_begin(struct bt_conn *conn)
{
	return host_ble_session_smp_begin((struct host_ble_link *)conn);
}

static void request_end(struct bt_conn *conn, int result)
{
	host_ble_session_smp_end();
}

static const struct smp_bt_request_callbacks request_callbacks = {
	.begin = request_begin, .end = request_end,
};
#endif

static int advertise(void)
{
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	bool start = host_ble_session_profile() == HOST_BLE_WAITING &&
		atomic_cas(&advertising, 0, 1);
	k_spin_unlock(&session_lock, key);
	if (!start) return 0;
	/* Publish before issuing HCI: a connection callback may arrive before
	 * bt_le_adv_start() returns and must be able to clear this reservation. */
#if !defined(CONFIG_MCUMGR_TRANSPORT_BT)
	uint8_t pnp[2 + PBIO_PYBRICKS_PNP_ID_SIZE];
	sys_put_le16(PBIO_GATT_PNP_ID_CHAR_UUID, pnp);
	pbio_pybricks_pnp_id(pnp + 2, host_protocol_hub_kind(), host_hub_variant());
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
	/* Name the shared identity explicitly: legacy scanning must not assume
	 * this advertiser has already initialized a private random address. */
	const struct bt_le_adv_param *params = BT_LE_ADV_PARAM(
		BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
		BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);
	int err = bt_le_adv_start(params, data, ARRAY_SIZE(data),
		response, ARRAY_SIZE(response));
	if (err) atomic_clear(&advertising);
	return err;
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

int host_radio_failure(const char *operation, int error)
{
	return radio_failed(operation, error);
}

static void disconnect_peer(struct k_work *work)
{
	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&session_lock);
		size_t index = 0;
		while (index < ARRAY_SIZE(disconnect_pending) && !disconnect_pending[index]) index++;
		struct bt_conn *conn = index < ARRAY_SIZE(disconnect_pending) ? disconnect_pending[index] : NULL;
		k_spin_unlock(&session_lock, key);
		if (!conn) return;

		k_mutex_lock(&radio_lock, K_FOREVER);
		int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if (err && err != -ENOTCONN) {
			radio_failed("disconnect", err);
		}
		k_mutex_unlock(&radio_lock);

		key = k_spin_lock(&session_lock);
		disconnect_pending[index] = NULL;
		k_spin_unlock(&session_lock, key);
		bt_conn_unref(conn);
	}
}
static K_WORK_DEFINE(disconnect_work, disconnect_peer);

static void request_disconnect(struct bt_conn *conn)
{
	/* Retain each distinct connection until its HCI command finishes. HCI
	 * waits must not block the system/button queue or Bluetooth RX callbacks.
	 * Only the real disconnection callback releases management ownership. */
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	size_t free = ARRAY_SIZE(disconnect_pending);
	for (size_t i = 0; i < ARRAY_SIZE(disconnect_pending); i++) {
		if (disconnect_pending[i] == conn) {
			k_spin_unlock(&session_lock, key);
			return;
		}
		if (!disconnect_pending[i]) free = i;
	}
	/* Every occupied entry holds a reference to a different Zephyr slot,
	 * so a new connection always has room in this equally sized array. */
	__ASSERT_NO_MSG(free < ARRAY_SIZE(disconnect_pending));
	disconnect_pending[free] = bt_conn_ref(conn);
	k_work_submit_to_queue(&radio_queue, &disconnect_work);
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
			if (!enable && !err) atomic_clear(&advertising);
		}
		if (!err && enable) {
			operation = "network initialization";
			err = hub_network_start();
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
			}
			k_spin_unlock(&observe_lock, key);
			host_radio_schedule(K_NO_WAIT);
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

static void print_connection(struct bt_conn *conn, void *data)
{
	struct bt_conn_info info;
	if (bt_conn_get_info(conn, &info) || info.state != BT_CONN_STATE_CONNECTED) return;
	shell_print(data, "BLE %s: interval %u us, latency %u, ATT MTU %u",
		info.role == BT_CONN_ROLE_PERIPHERAL ? "host" : "peer",
		info.le.interval_us, info.le.latency, bt_gatt_get_mtu(conn));
}

static int cmd_bluetooth(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		const char *const names[] = {"off", "starting", "on", "stopping", "failed"};
		int state = atomic_get(&radio_state);
		shell_print(shell, "Bluetooth: %s", names[state]);
		if (state == RADIO_FAILED) {
			shell_print(shell, "%s failed: %d; reboot required", failed_operation, radio_error);
		}
		if (state == RADIO_ON) bt_conn_foreach(BT_CONN_TYPE_LE, print_connection, (void *)shell);
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
	k_work_queue_start(&radio_queue, radio_stack, K_KERNEL_STACK_SIZEOF(radio_stack),
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
