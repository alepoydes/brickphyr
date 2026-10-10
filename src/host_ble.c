// SPDX-License-Identifier: Apache-2.0
#include "host_protocol.h"
#include "host_transport.h"

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
static struct k_spinlock session_lock;
static bool exit_compatibility;
K_MUTEX_DEFINE(radio_lock);
static bool advertising_enabled;

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
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, length);
}

static ssize_t write_command(struct bt_conn *conn, const struct bt_gatt_attr *attr,
	const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (conn != owner) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (offset || (flags & BT_GATT_WRITE_FLAG_PREPARE)) {
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}
	uint8_t error = host_protocol_command(buf, len);
	if (error) {
		return BT_GATT_ERR(error);
	}
	/* The sole supported command selects Shell after the link closes. */
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	exit_compatibility = true;
	k_spin_unlock(&session_lock, key);
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
	return value <= BT_GATT_CCC_NOTIFY ? sizeof(value) :
		BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
}

static void subscribed(const struct bt_gatt_attr *attr, uint16_t value)
{
	if (value != BT_GATT_CCC_NOTIFY || !owner) {
		return;
	}
	uint8_t status[PBIO_PYBRICKS_EVENT_STATUS_REPORT_SIZE];
	pbio_pybricks_event_status_report(status,
		BIT(PBIO_PYBRICKS_STATUS_BLE_HOST_CONNECTED), 0, 0);
	/* The value attribute immediately precedes its CCC descriptor. */
	int err = bt_gatt_notify(owner, attr - 1, status, sizeof(status));
	if (err) {
		LOG_ERR("Initial Pybricks status failed: %d", err);
	}
}

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

/* Existing clients subscribe even when no application can produce stdout.
 * Reject stdin explicitly until an execution engine exists. */
BT_GATT_SERVICE_DEFINE(legacy_uart,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(BT_UUID_NUS_SRV_VAL)),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BT_UUID_NUS_TX_CHAR_VAL),
		BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC_WITH_WRITE_CB(NULL, allow_subscription,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BT_UUID_NUS_RX_CHAR_VAL),
		BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, reject_stdin, NULL));

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		return;
	}
	if (host_ble_acquire()) {
		int result = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if (result) {
			LOG_ERR("Reject occupied BLE connection failed: %d", result);
		}
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	owner = bt_conn_ref(conn);
	exit_compatibility = false;
	k_spin_unlock(&session_lock, key);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	bool owned = conn == owner;
	if (owned) {
		owner = NULL;
		/* Keep the shared owner/power hold until any accepted packet finishes.
		 * Do not block RX: the SMP sender may need its disconnect callback. */
		host_ble_release(exit_compatibility);
		exit_compatibility = false;
	}
	k_spin_unlock(&session_lock, key);
	if (owned) {
		bt_conn_unref(conn);
	}
}

#if defined(CONFIG_MCUMGR_TRANSPORT_BT)
static bool request_begin(struct bt_conn *conn)
{
	k_spinlock_key_t key = k_spin_lock(&session_lock);
	bool valid = owner == conn && host_ble_request_begin();
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

static void resume_advertising(struct k_work *work)
{
	k_mutex_lock(&radio_lock, K_FOREVER);
	if (advertising_enabled) {
		int err = advertise();
		if (err) {
			LOG_ERR("Resume advertising failed: %d", err);
		}
	}
	k_mutex_unlock(&radio_lock);
}
K_WORK_DEFINE(advertising_work, resume_advertising);

static void recycled(void)
{
	k_work_submit(&advertising_work);
}

BT_CONN_CB_DEFINE(host_ble_callbacks) = {
	.connected = connected, .disconnected = disconnected, .recycled = recycled,
};

static int cmd_bluetooth(const struct shell *shell, size_t argc, char **argv)
{
	bool enable = !strcmp(argv[1], "on");
	if (!enable && strcmp(argv[1], "off")) {
		return -EINVAL;
	}
	k_mutex_lock(&radio_lock, K_FOREVER);
	int err;
	if (enable) {
		err = bt_enable(NULL);
		if (!err || err == -EALREADY) {
			err = advertising_enabled ? 0 : advertise();
			if (!err) {
				advertising_enabled = true;
			}
		}
	} else {
		advertising_enabled = false;
		err = bt_le_adv_stop();
		if (!err) {
			err = bt_disable();
		}
	}
	k_mutex_unlock(&radio_lock);
	if (err) {
		shell_error(shell, "Bluetooth %s failed: %d", argv[1], err);
	} else {
		shell_print(shell, "%s", enable ?
			"Bluetooth on. Close USB before connecting over BLE." : "Bluetooth off.");
	}
	return err;
}
SHELL_CMD_ARG_REGISTER(bluetooth, NULL, "Bluetooth management: on | off.", cmd_bluetooth, 2, 0);

static int setup(void)
{
	/* PBIO exports canonical UUID bytes; Zephyr stores their wire order. */
	sys_memcpy_swap(service_uuid.val, pbio_pybricks_service_uuid, sizeof(service_uuid.val));
	sys_memcpy_swap(command_uuid.val, pbio_pybricks_command_event_char_uuid,
		sizeof(command_uuid.val));
	sys_memcpy_swap(capabilities_uuid.val, pbio_pybricks_hub_capabilities_char_uuid,
		sizeof(capabilities_uuid.val));
#if defined(CONFIG_MCUMGR_TRANSPORT_BT)
	return smp_bt_register_request_callbacks(&request_callbacks);
#else
	return 0;
#endif
}
SYS_INIT(setup, APPLICATION, 90);
