// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2024 The Pybricks Authors
/* LWP3 discovery, Remote notifications and message limits adapted from
 * pybricks/iodevices/pb_type_iodevices_lwp3device.c at a7b1b04ae.
 * Zephyr owns connections/ATT; the shared radio worker owns all commands. */
#include "hub_lwp3.h"
#include "host_radio.h"
#include "lwp3_devices.h"
#include <string.h>
#include <lego/lwp3.h>
#include <pbio/color_map.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/byteorder.h>

static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x00001623, 0x1212, 0xefde, 0x1623, 0x785feabcd123));
static struct bt_uuid_128 characteristic_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x00001624, 0x1212, 0xefde, 0x1623, 0x785feabcd123));

enum phase { IDLE, SCAN, CREATE, SECURITY, SERVICE, CHARACTERISTIC, CCC, SUBSCRIBE, SETUP, READY, WRITE, CLOSING };
static struct k_spinlock lock;
static uint32_t sequence;
static struct {
	uint32_t handle;
	struct bp_lwp3_config config;
	struct bp_lwp3_state state;
	struct bt_conn *conn;
	enum phase phase;
	bool pending, cancel, ended, candidate, opening;
	bool paired, configured, drive_request;
	uint8_t setup_step, drive_repeats;
	int64_t drive_ms;
	uint8_t drive[2];
	struct bp_sensor_palette palette;
	bt_addr_le_t address;
	int64_t deadline;
	uint16_t service_start, service_end, value_handle, ccc_handle;
	struct bt_gatt_discover_params discovery;
	struct bt_gatt_subscribe_params subscription;
	struct bt_gatt_write_params write;
	struct bp_lwp3_message tx, *messages;
	uint16_t first, count;
} peer;

static void wake(void) { host_radio_schedule(K_NO_WAIT); }
static int lease(uint32_t handle) { return handle && handle == peer.handle ? 0 : -ESTALE; }

/* Called with lock held. Keep the initiating error through asynchronous cleanup. */
static void fail(int error)
{
	bool changed = !peer.cancel;
	if (!peer.state.error) peer.state.error = error;
	peer.state.connected = peer.state.busy = 0;
	peer.cancel = true;
	if (changed) wake();
}

int hub_lwp3_open(const struct bp_lwp3_config *config, uint32_t *handle)
{
	if (config->timeout_ms > INT32_MAX || !memchr(config->name, 0, sizeof(config->name)) ||
	    config->flags & ~BP_LWP3_PAIR || config->profile > BP_LWP3_DUPLO ||
	    (config->profile && (config->kind || config->flags))) return -EINVAL;
	if (!IS_ENABLED(CONFIG_BT_SMP) && ((config->flags & BP_LWP3_PAIR) ||
	    config->profile == BP_LWP3_MARIO || config->profile == BP_LWP3_TECHNIC_MOVE)) return -ENOTSUP;
	int error = host_radio_error();
	if (error) return error;
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool reuse = peer.phase == READY && !peer.cancel && peer.state.connected &&
		lwp3_device_matches(config, peer.state.kind) &&
		(!lwp3_device_pair(config, peer.state.kind) || peer.paired) &&
		!strncmp(peer.state.name, config->name, strlen(config->name));
	if (peer.handle || (peer.phase != IDLE && !reuse) || (peer.conn && !reuse)) error = -EBUSY;
	else {
		/* Publication is nonblocking and atomic with release/notifications.
		 * An aborted syscall cannot leak an unpublished allocation. */
		struct bp_lwp3_message *messages = config->notifications ?
			k_calloc(config->notifications, sizeof(*messages)) : NULL;
		if (config->notifications && !messages) error = -ENOMEM;
		else {
			peer.config = *config;
			peer.messages = messages;
			peer.first = peer.count = 0;
			peer.state.buttons = 0;
			peer.state.fields = 0;
			peer.state.error = 0;
			peer.configured = false;
			peer.drive_repeats = 0;
			pbio_color_map_t map;
			pbio_color_map_set_default(&map);
			peer.palette = (struct bp_sensor_palette){.count = map.num_colors};
			for (size_t i = 0; i < map.num_colors; i++) peer.palette.colors[i] = map.colors[i];
			if (!reuse) {
				memset(&peer.state, 0, sizeof(peer.state));
				peer.cancel = false;
			}
			if (!++sequence) ++sequence;
			*handle = peer.handle = sequence;
		}
	}
	k_spin_unlock(&lock, key);
	return error;
}

int hub_lwp3_connect(uint32_t handle)
{
	int error = host_radio_error();
	if (error) return error;
	k_spinlock_key_t key = k_spin_lock(&lock);
	error = lease(handle);
	if (!error && (peer.cancel || (peer.phase != IDLE && peer.phase != READY))) error = -EBUSY;
	if (!error) {
		peer.state.error = 0;
		if (peer.phase == READY && !peer.configured) {
			peer.phase = SETUP;
			peer.setup_step = 0;
			peer.state.busy = 1;
			wake();
		} else if (peer.phase != READY) {
			peer.phase = SCAN;
			peer.state.busy = 1;
			peer.state.buttons = 0;
			peer.state.fields = 0;
			peer.paired = peer.configured = false;
			peer.drive_repeats = 0;
			peer.state.name[0] = 0;
			peer.first = peer.count = 0;
			peer.candidate = false;
			peer.deadline = peer.config.timeout_ms ? k_uptime_get() + peer.config.timeout_ms : 0;
			wake();
		}
	}
	k_spin_unlock(&lock, key);
	return error;
}

int hub_lwp3_disconnect(uint32_t handle)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = lease(handle);
	if (!error) {
		peer.state.error = 0;
		peer.state.busy = 1;
		peer.state.connected = 0;
		peer.cancel = true;
		wake();
	}
	k_spin_unlock(&lock, key);
	return error;
}

int hub_lwp3_get(uint32_t handle, struct bp_lwp3_state *state)
{
	struct bp_sensor_palette palette;
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = lease(handle);
	if (!error) {
		*state = peer.state;
		palette = peer.palette;
	}
	k_spin_unlock(&lock, key);
	if (!error) lwp3_device_color(state, &palette);
	return error;
}

/* Called with lock held, after readiness and data validation. */
static void queue_write(const void *data, size_t size)
{
	memcpy(peer.tx.data, data, size);
	peer.tx.size = size;
	peer.state.error = 0;
	peer.state.busy = 1;
	peer.phase = WRITE;
	peer.drive_request = false;
	wake();
}

int hub_lwp3_write(uint32_t handle, const void *data, size_t size)
{
	if (size < 3 || size > BP_LWP3_MESSAGE_MAX || ((const uint8_t *)data)[0] != size) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = lease(handle);
	if (!error && !peer.state.connected) error = -ENODEV;
	if (!error && peer.phase != READY) error = -EBUSY;
	if (!error) queue_write(data, size);
	k_spin_unlock(&lock, key);
	return error;
}

int hub_lwp3_command(uint32_t handle, uint32_t command, int32_t value, int32_t auxiliary)
{
	uint8_t data[BP_LWP3_MESSAGE_MAX];
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = lease(handle);
	if (!error && !peer.state.connected) error = -ENODEV;
	if (!error && peer.phase != READY) error = -EBUSY;
	if (!error) {
		int size = lwp3_device_command(peer.config.profile, peer.state.kind,
			command, value, auxiliary, data);
		if (size < 0) error = size;
		else {
			bool drive = peer.config.profile == BP_LWP3_TECHNIC_MOVE && command == BP_LWP3_DRIVE;
			bool repeat = drive && peer.drive_repeats && data[9] == peer.drive[0] &&
				data[10] == peer.drive[1] && k_uptime_get() - peer.drive_ms < 500;
			if (repeat && peer.drive_repeats == 2) peer.state.error = 0;
			else {
				queue_write(data, size);
				peer.drive_request = drive;
				if (drive && !repeat) peer.drive_repeats = 0;
			}
		}
	}
	k_spin_unlock(&lock, key);
	return error;
}

int hub_lwp3_palette_get(uint32_t handle, struct bp_sensor_palette *palette)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = lease(handle);
	if (!error && peer.config.profile != BP_LWP3_MARIO) error = -ENOTSUP;
	if (!error) *palette = peer.palette;
	k_spin_unlock(&lock, key);
	return error;
}

int hub_lwp3_palette_set(uint32_t handle, const struct bp_sensor_palette *palette)
{
	if (palette->count > BP_SENSOR_PALETTE_SIZE) return -EINVAL;
	for (size_t i = 0; i < palette->count; i++) {
		pbio_color_t color = palette->colors[i];
		if (pbio_color_get_h(color) >= 360 || pbio_color_get_s(color) > 100 ||
		    pbio_color_get_v(color) < -100 || pbio_color_get_v(color) > 100) return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = lease(handle);
	if (!error && peer.config.profile != BP_LWP3_MARIO) error = -ENOTSUP;
	if (!error) peer.palette = *palette;
	k_spin_unlock(&lock, key);
	return error;
}

int hub_lwp3_read(uint32_t handle, struct bp_lwp3_message *message)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = lease(handle);
	if (!error && !peer.state.connected) error = -ENODEV;
	if (!error) {
		memset(message, 0, sizeof(*message));
		if (peer.count) {
			*message = peer.messages[peer.first];
			peer.first = (peer.first + 1) % peer.config.notifications;
			peer.count--;
			error = message->size;
		}
	}
	k_spin_unlock(&lock, key);
	return error;
}

static int release(uint32_t handle, bool force)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (force || (handle && peer.handle == handle)) {
		peer.handle = 0;
		k_free(peer.messages);
		peer.messages = NULL;
		peer.first = peer.count = 0;
		if (force || peer.phase != READY) peer.cancel = true;
		wake();
	}
	k_spin_unlock(&lock, key);
	return 0;
}

int hub_lwp3_close(uint32_t handle) { return release(handle, false); }
int hub_lwp3_release(void) { return release(0, true); }

int hub_lwp3_scan_mode(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int mode = peer.phase == CREATE ? 2 : peer.phase == SCAN && !peer.cancel ? 1 : 0;
	k_spin_unlock(&lock, key);
	return mode;
}

void hub_lwp3_receive(const bt_addr_le_t *address, uint8_t type, const uint8_t *data, size_t size)
{
	if (!address || (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_SCAN_RSP)) return;
	bool service = false, kind = false;
	uint8_t selected_kind = 0;
	const uint8_t *name = NULL;
	size_t name_size = 0;
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (peer.phase != SCAN || peer.cancel) goto out;
	/* Parse the complete AD packet before publishing a candidate. Malformed
	 * packets and scan responses from another address cannot select a peer. */
	while (size && data[0]) {
		size_t length = data[0];
		if (length >= size) goto out;
		const uint8_t *value = data + 2;
		size_t count = length - 1;
		if ((data[1] == BT_DATA_UUID128_ALL || data[1] == BT_DATA_UUID128_SOME) && count % 16 == 0) {
			for (size_t i = 0; i < count; i += 16) {
				if (!memcmp(value + i, service_uuid.val, 16)) service = true;
			}
		} else if (data[1] == BT_DATA_MANUFACTURER_DATA && count == 8 &&
			   sys_get_le16(value) == LWP3_LEGO_COMPANY_ID && lwp3_device_matches(&peer.config, value[3])) {
			kind = true;
			selected_kind = value[3];
		} else if (data[1] == BT_DATA_NAME_COMPLETE || data[1] == BT_DATA_NAME_SHORTENED) {
			name = value;
			name_size = count;
		}
		data += length + 1;
		size -= length + 1;
	}
	if (type == BT_GAP_ADV_TYPE_ADV_IND) {
		if (!service || !kind) goto out;
		peer.address = *address;
		peer.state.kind = selected_kind;
		peer.candidate = true;
	} else if (!peer.candidate || bt_addr_le_cmp(address, &peer.address)) goto out;
	if (!name) goto out;
	size_t filter = strlen(peer.config.name);
	if (name_size < filter || memcmp(name, peer.config.name, filter)) {
		peer.candidate = false;
		goto out;
	}
	if (!IS_ENABLED(CONFIG_BT_SMP) && lwp3_device_pair(&peer.config, peer.state.kind)) {
		fail(-ENOTSUP);
		goto out;
	}
	name_size = MIN(name_size, BP_LWP3_NAME_MAX);
	memcpy(peer.state.name, name, name_size);
	peer.state.name[name_size] = 0;
	memcpy(peer.config.name, peer.state.name, sizeof(peer.config.name));
	peer.phase = CREATE;
	peer.deadline = 0;
	wake();
out:
	k_spin_unlock(&lock, key);
}

static uint8_t discovered(struct bt_conn *conn, const struct bt_gatt_attr *attr,
	struct bt_gatt_discover_params *params)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn != peer.conn || peer.cancel) goto done;
	peer.pending = false;
	if (!attr) fail(-ENOENT);
	else if (peer.phase == SERVICE) {
		const struct bt_gatt_service_val *value = attr->user_data;
		peer.service_start = attr->handle;
		peer.service_end = value->end_handle;
		if (peer.service_end <= peer.service_start) fail(-EPROTO);
		else peer.phase = CHARACTERISTIC;
	} else if (peer.phase == CHARACTERISTIC) {
		const struct bt_gatt_chrc *value = attr->user_data;
		uint8_t needed = BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY;
		if ((value->properties & needed) != needed || value->value_handle <= attr->handle ||
		    value->value_handle >= peer.service_end) fail(-ENOTSUP);
		else {
			peer.value_handle = value->value_handle;
			peer.phase = CCC;
		}
	} else if (peer.phase == CCC) {
		peer.ccc_handle = attr->handle;
		peer.phase = SUBSCRIBE;
	}
	peer.deadline = 0;
	wake();
done:
	k_spin_unlock(&lock, key);
	return BT_GATT_ITER_STOP;
}

static uint8_t notified(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
	const void *data, uint16_t size)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn != peer.conn || peer.cancel) goto done;
	if (!data) {
		fail(-ENOTCONN);
		goto done;
	}
	const uint8_t *value = data;
	if (!peer.handle || size < 3 || size > BP_LWP3_MESSAGE_MAX || value[0] != size) goto done;
	if (peer.config.profile != BP_LWP3_RAW) lwp3_device_notify(&peer.state, data, size, k_uptime_get());
	if (peer.config.notifications) {
		unsigned int index = (peer.first + peer.count) % peer.config.notifications;
		peer.messages[index].size = size;
		memcpy(peer.messages[index].data, data, size);
		if (peer.count < peer.config.notifications) peer.count++;
		else peer.first = (peer.first + 1) % peer.config.notifications;
	}
done:
	k_spin_unlock(&lock, key);
	return BT_GATT_ITER_CONTINUE;
}

static void subscribed(struct bt_conn *conn, uint8_t error, struct bt_gatt_subscribe_params *params)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn == peer.conn && peer.phase == SUBSCRIBE && !peer.cancel) {
		peer.pending = false;
		peer.deadline = 0;
		if (error) fail(-EIO);
		else {
			peer.phase = SETUP;
			peer.setup_step = 0;
			peer.state.connected = 1;
			peer.state.busy = 1;
		}
		wake();
	}
	k_spin_unlock(&lock, key);
}

static void written(struct bt_conn *conn, uint8_t error, struct bt_gatt_write_params *params)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn == peer.conn && (peer.phase == WRITE || peer.phase == SETUP) && !peer.cancel) {
		peer.pending = false;
		peer.deadline = 0;
		if (peer.phase == SETUP) {
			if (error) fail(-EIO);
			else peer.setup_step++;
			wake();
			goto done;
		}
		peer.state.error = error ? -EIO : 0;
		peer.state.busy = 0;
		peer.phase = READY;
		if (peer.drive_request) {
			if (error) peer.drive_repeats = 0;
			else {
				peer.drive[0] = peer.tx.data[9];
				peer.drive[1] = peer.tx.data[10];
				peer.drive_repeats++;
				peer.drive_ms = k_uptime_get();
			}
		}
		const uint8_t *value = peer.tx.data;
		if (!error && peer.tx.size > 5 && peer.tx.size <= 5 + BP_LWP3_NAME_MAX &&
		    value[2] == LWP3_MSG_TYPE_HUB_PROPERTIES && value[3] == LWP3_HUB_PROPERTY_NAME &&
		    value[4] == LWP3_HUB_PROPERTY_OP_SET) {
			memcpy(peer.state.name, value + 5, peer.tx.size - 5);
			peer.state.name[peer.tx.size - 5] = 0;
			memcpy(peer.config.name, peer.state.name, sizeof(peer.config.name));
		}
		wake();
	}
done:
	k_spin_unlock(&lock, key);
}

/* There is one application central initiator. During the public create call,
 * connected may run before the returned reference has been published. The
 * radio worker cannot run again until that call finishes; callbacks only copy
 * state, and never release or replace its connection reference. */
static bool our_connection(struct bt_conn *conn)
{
	if (peer.conn) return conn == peer.conn;
	struct bt_conn_info info;
	return peer.opening && !bt_conn_get_info(conn, &info) &&
		info.type == BT_CONN_TYPE_LE && info.role == BT_CONN_ROLE_CENTRAL;
}

static void connected(struct bt_conn *conn, uint8_t error)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (our_connection(conn)) {
		peer.pending = false;
		peer.deadline = 0;
		if (error) {
			peer.ended = true;
			if (!peer.cancel) fail(error == BT_HCI_ERR_UNKNOWN_CONN_ID ? -ETIMEDOUT : -ECONNREFUSED);
		} else peer.phase = lwp3_device_pair(&peer.config, peer.state.kind) ? SECURITY : SERVICE;
		wake();
	}
	k_spin_unlock(&lock, key);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (our_connection(conn)) {
		peer.ended = true;
		if (!peer.cancel) fail(-ENOTCONN);
		wake();
	}
	k_spin_unlock(&lock, key);
}

#ifdef CONFIG_BT_SMP
static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err error)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn == peer.conn && peer.phase == SECURITY && !peer.cancel) {
		peer.pending = false;
		peer.deadline = 0;
		if (error || level < BT_SECURITY_L2) fail(-EACCES);
		else {
			peer.paired = true;
			peer.phase = SERVICE;
		}
		wake();
	}
	k_spin_unlock(&lock, key);
}
#endif

BT_CONN_CB_DEFINE(lwp3_callbacks) = {
	.connected = connected, .disconnected = disconnected,
#ifdef CONFIG_BT_SMP
	.security_changed = security_changed,
#endif
};

void hub_lwp3_process(int radio_error, bool scan_stopped)
{
	struct bt_conn *retired = NULL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (radio_error && peer.phase != IDLE) fail(radio_error);
	if (peer.ended) {
		/* In pinned Zephyr, ATT request and subscription teardown precedes
		 * disconnected. Params can be reused only after this event, not after
		 * bt_gatt_cancel()/bt_conn_disconnect() returns. */
		retired = peer.conn;
		peer.conn = NULL;
		peer.ended = peer.pending = false;
		peer.phase = IDLE;
		peer.state.connected = peer.state.busy = 0;
	}
	if (peer.deadline && k_uptime_get() >= peer.deadline) fail(-ETIMEDOUT);
	if (peer.cancel) {
		if (!peer.conn) {
			peer.phase = IDLE;
			peer.state.connected = peer.state.busy = 0;
			peer.pending = peer.cancel = false;
			peer.deadline = 0;
		} else if (peer.phase != CLOSING) {
			peer.phase = CLOSING;
			peer.pending = false;
			peer.deadline = 0;
		}
	}
	if (peer.phase == SETUP && !peer.pending && !peer.cancel) {
		int size = lwp3_device_setup(peer.config.profile, peer.state.kind, peer.setup_step, peer.tx.data);
		if (size < 0) fail(size);
		else if (!size) {
			peer.configured = true;
			peer.phase = READY;
			peer.state.busy = 0;
		} else peer.tx.size = size;
	}
	enum phase phase = peer.phase;
	struct bt_conn *conn = peer.conn;
	bool issue = !radio_error && !peer.pending && (!peer.cancel || phase == CLOSING) &&
		phase != IDLE && phase != SCAN && phase != READY;
	if (issue && phase == CREATE && !scan_stopped) {
		/* RX may find a candidate while scan_start itself is completing. */
		issue = false;
		wake();
	}
	if (issue) {
		peer.pending = true;
		/* ATT's transaction timeout is 30 s; pairing has the reference 5 s
		 * bound. Creation uses Zephyr's own 5 s create parameter below. */
		peer.deadline = phase == CLOSING || phase == CREATE ? 0 :
			k_uptime_get() + (phase == SECURITY ? 5000 : 30000);
	}
	int64_t deadline = peer.deadline;
	k_spin_unlock(&lock, key);
	if (retired) bt_conn_unref(retired);
	if (deadline) host_radio_schedule(K_MSEC(MAX(1, deadline - k_uptime_get())));
	if (!issue) return;

	int error = 0;
	switch (phase) {
	case CREATE: {
		const struct bt_conn_le_create_param create = {.interval = 0x30, .window = 0x30, .timeout = 500};
		struct bt_conn *created = NULL;
		key = k_spin_lock(&lock);
		peer.opening = true;
		k_spin_unlock(&lock, key);
		error = bt_conn_le_create(&peer.address, &create, BT_LE_CONN_PARAM_DEFAULT, &created);
		key = k_spin_lock(&lock);
		if (!error) peer.conn = created;
		peer.opening = false;
		k_spin_unlock(&lock, key);
		break;
	}
	case SECURITY:
#ifdef CONFIG_BT_SMP
		error = bt_conn_set_security(conn, BT_SECURITY_L2 | BT_SECURITY_FORCE_PAIR);
#else
		error = -ENOTSUP;
#endif
		break;
	case SERVICE:
	case CHARACTERISTIC:
	case CCC:
		peer.discovery = (struct bt_gatt_discover_params){
			.uuid = phase == SERVICE ? &service_uuid.uuid :
				phase == CHARACTERISTIC ? &characteristic_uuid.uuid : BT_UUID_GATT_CCC,
			.func = discovered,
			.start_handle = phase == SERVICE ? BT_ATT_FIRST_ATTRIBUTE_HANDLE :
				phase == CHARACTERISTIC ? peer.service_start + 1 : peer.value_handle + 1,
			.end_handle = phase == SERVICE ? BT_ATT_LAST_ATTRIBUTE_HANDLE : peer.service_end,
			.type = phase == SERVICE ? BT_GATT_DISCOVER_PRIMARY :
				phase == CHARACTERISTIC ? BT_GATT_DISCOVER_CHARACTERISTIC : BT_GATT_DISCOVER_DESCRIPTOR,
		};
		error = bt_gatt_discover(conn, &peer.discovery);
		break;
	case SUBSCRIBE:
		peer.subscription = (struct bt_gatt_subscribe_params){
			.notify = notified, .subscribe = subscribed, .value = BT_GATT_CCC_NOTIFY,
			.value_handle = peer.value_handle, .ccc_handle = peer.ccc_handle,
		};
		atomic_set_bit(peer.subscription.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
		error = bt_gatt_subscribe(conn, &peer.subscription);
		break;
	case WRITE:
	case SETUP:
		peer.write = (struct bt_gatt_write_params){.func = written,
			.handle = peer.value_handle, .data = peer.tx.data, .length = peer.tx.size};
		error = bt_gatt_write(conn, &peer.write);
		break;
	case CLOSING:
		error = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		/* ENOTCONN may precede the deferred disconnected callback. Retain the
		 * reference/params until that callback; do not manufacture completion. */
		if (error == -ENOTCONN) error = 0;
		break;
	default:
		break;
	}
	if (error) {
		if (phase == CLOSING) host_radio_failure("LEGO disconnect", error);
		key = k_spin_lock(&lock);
		if (phase != CLOSING) peer.pending = false;
		if (phase == WRITE && peer.phase == WRITE && !peer.cancel) {
			if (peer.drive_request) peer.drive_repeats = 0;
			peer.phase = READY;
			peer.state.error = error;
			peer.state.busy = 0;
			peer.deadline = 0;
		} else fail(error);
		k_spin_unlock(&lock, key);
	}
}
