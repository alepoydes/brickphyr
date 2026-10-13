// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2024 The Pybricks Authors
/* LWP3 discovery, Remote notifications and message limits adapted from
 * pybricks/iodevices/pb_type_iodevices_lwp3device.c at a7b1b04ae.
 * Zephyr owns connections/ATT; the shared radio worker owns all commands. */
#include "hub_ble_peer.h"
#include "host_radio.h"
#include "lwp3_devices.h"
#include "xbox_device.h"
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

enum phase { IDLE, SCAN, CREATE, SECURITY, SERVICE, CHARACTERISTIC, CCC,
	HID_DESCRIPTOR, HID_REFERENCE, SUBSCRIBE, SETUP, READY, WRITE, CLOSING };
/* HID services contain several Report characteristics with the same UUID.
 * Discover their bounded descriptor ranges and select by Report Reference. */
struct hid_report { uint16_t handle, end, reference, ccc; uint8_t properties; };
static struct k_spinlock lock;
static uint32_t sequence;
static struct {
	uint32_t handle;
	struct bp_ble_peer_config config;
	struct bp_ble_peer_state state;
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
	uint16_t service_start, service_end, value_handle, output_handle, ccc_handle;
	struct hid_report reports[8];
	uint8_t report_count, report_index, reference_size, reference[2];
	struct bt_gatt_discover_params discovery;
	struct bt_gatt_read_params read;
	struct bt_gatt_subscribe_params subscription;
	struct bt_gatt_write_params write;
	struct bp_lwp3_message tx, *messages;
	uint16_t first, count;
} peer;

static void wake(void) { host_radio_schedule(K_NO_WAIT); }
static int lease(uint32_t handle) { return handle && handle == peer.handle ? 0 : -ESTALE; }
static bool xbox(void) { return peer.config.protocol == BP_BLE_XBOX; }
static bool needs_pairing(void)
{
	return xbox() || lwp3_device_pair(&peer.config.lwp3, peer.state.lwp3.kind);
}
static int lego_lease(uint32_t handle)
{
	int error = lease(handle);
	return error ? error : xbox() ? -ENOTSUP : 0;
}
static void invalidate_sample(void)
{
	if (xbox()) peer.state.xbox = (struct bp_xbox_state){.error = -EAGAIN};
	else {
		peer.state.lwp3.buttons = 0;
		peer.state.lwp3.fields = 0;
	}
}

/* Called with lock held. Keep the initiating error through asynchronous cleanup. */
static void fail(int error)
{
	bool changed = !peer.cancel;
	if (!peer.state.error) peer.state.error = error;
	peer.state.connected = peer.state.busy = 0;
	peer.cancel = true;
	if (changed) wake();
}

int hub_ble_peer_open(const struct bp_ble_peer_config *config, uint32_t *handle)
{
	if (config->timeout_ms > INT32_MAX || !memchr(config->name, 0, sizeof(config->name)) ||
	    config->protocol > BP_BLE_XBOX) return -EINVAL;
	bool is_xbox = config->protocol == BP_BLE_XBOX;
	if (is_xbox ? config->xbox_deadzone > 100 :
	    (strlen(config->name) > BP_LWP3_NAME_MAX || config->lwp3.flags & ~BP_LWP3_PAIR ||
	     config->lwp3.profile > BP_LWP3_DUPLO ||
	     (config->lwp3.profile && (config->lwp3.kind || config->lwp3.flags)))) return -EINVAL;
	if (!IS_ENABLED(CONFIG_BT_SMP) && (is_xbox || (config->lwp3.flags & BP_LWP3_PAIR) ||
	    config->lwp3.profile == BP_LWP3_MARIO || config->lwp3.profile == BP_LWP3_TECHNIC_MOVE)) return -ENOTSUP;
	int error = host_radio_error();
	if (error) return error;
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool reuse = peer.phase == READY && !peer.cancel && peer.state.connected &&
		peer.config.protocol == config->protocol &&
		(is_xbox || lwp3_device_matches(&config->lwp3, peer.state.lwp3.kind)) &&
		(!(is_xbox || lwp3_device_pair(&config->lwp3, peer.state.lwp3.kind)) || peer.paired) &&
		!strncmp(peer.state.name, config->name, strlen(config->name));
	if (peer.handle || (peer.phase != IDLE && !reuse) || (peer.conn && !reuse)) error = -EBUSY;
	else {
		/* Publication is nonblocking and atomic with release/notifications.
		 * An aborted syscall cannot leak an unpublished allocation. */
		uint8_t notifications = is_xbox ? 0 : config->lwp3.notifications;
		struct bp_lwp3_message *messages = notifications ?
			k_calloc(notifications, sizeof(*messages)) : NULL;
		if (notifications && !messages) error = -ENOMEM;
		else {
			peer.config = *config;
			peer.messages = messages;
			peer.first = peer.count = 0;
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
			invalidate_sample();
			if (!++sequence) ++sequence;
			*handle = peer.handle = sequence;
		}
	}
	k_spin_unlock(&lock, key);
	return error;
}

int hub_ble_peer_connect(uint32_t handle)
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
			invalidate_sample();
			peer.report_count = peer.report_index = 0;
			peer.value_handle = peer.output_handle = peer.ccc_handle = 0;
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

int hub_ble_peer_disconnect(uint32_t handle)
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

int hub_ble_peer_get(uint32_t handle, struct bp_ble_peer_state *state)
{
	struct bp_sensor_palette palette;
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = lease(handle);
	bool is_xbox = xbox();
	if (!error) {
		*state = peer.state;
		palette = peer.palette;
	}
	k_spin_unlock(&lock, key);
	if (!error && !is_xbox) lwp3_device_color(&state->lwp3, &palette);
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

int hub_xbox_rumble(uint32_t handle, const struct bp_xbox_rumble *rumble)
{
	uint8_t data[8];
	int size = xbox_device_rumble(rumble, data);
	if (size < 0) return size;
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = lease(handle);
	if (!error && !xbox()) error = -ENOTSUP;
	if (!error && !peer.state.connected) error = -ENODEV;
	if (!error && peer.phase != READY) error = -EBUSY;
	if (!error) {
		if (size) queue_write(data, size);
		else peer.state.error = 0;
	}
	k_spin_unlock(&lock, key);
	return error;
}

int hub_lwp3_write(uint32_t handle, const void *data, size_t size)
{
	if (size < 3 || size > BP_LWP3_MESSAGE_MAX || ((const uint8_t *)data)[0] != size) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = lego_lease(handle);
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
	int error = lego_lease(handle);
	if (!error && !peer.state.connected) error = -ENODEV;
	if (!error && peer.phase != READY) error = -EBUSY;
	if (!error) {
		int size = lwp3_device_command(peer.config.lwp3.profile, peer.state.lwp3.kind,
			command, value, auxiliary, data);
		if (size < 0) error = size;
		else {
			bool drive = peer.config.lwp3.profile == BP_LWP3_TECHNIC_MOVE && command == BP_LWP3_DRIVE;
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
	int error = lego_lease(handle);
	if (!error && peer.config.lwp3.profile != BP_LWP3_MARIO) error = -ENOTSUP;
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
	int error = lego_lease(handle);
	if (!error && peer.config.lwp3.profile != BP_LWP3_MARIO) error = -ENOTSUP;
	if (!error) peer.palette = *palette;
	k_spin_unlock(&lock, key);
	return error;
}

int hub_lwp3_read(uint32_t handle, struct bp_lwp3_message *message)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = lego_lease(handle);
	if (!error && !peer.state.connected) error = -ENODEV;
	if (!error) {
		memset(message, 0, sizeof(*message));
		if (peer.count) {
			*message = peer.messages[peer.first];
			peer.first = (peer.first + 1) % peer.config.lwp3.notifications;
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

int hub_ble_peer_close(uint32_t handle) { return release(handle, false); }
int hub_ble_peer_release(void) { return release(0, true); }

int hub_ble_peer_scan_mode(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int mode = peer.phase == CREATE ? 2 : peer.phase == SCAN && !peer.cancel ? 1 : 0;
	k_spin_unlock(&lock, key);
	return mode;
}

void hub_ble_peer_receive(const bt_addr_le_t *address, uint8_t type, const uint8_t *data, size_t size)
{
	if (!address || (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_SCAN_RSP)) return;
	bool service = false, kind = false, gamepad = false, microsoft = false;
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
			   sys_get_le16(value) == LWP3_LEGO_COMPANY_ID && lwp3_device_matches(&peer.config.lwp3, value[3])) {
			kind = true;
			selected_kind = value[3];
		} else if (data[1] == BT_DATA_MANUFACTURER_DATA && count >= 2 &&
			   sys_get_le16(value) == 0x0006) {
			microsoft = true;
		} else if (data[1] == BT_DATA_GAP_APPEARANCE && count == 2 &&
			   sys_get_le16(value) == 964) {
			gamepad = true;
		} else if (data[1] == BT_DATA_NAME_COMPLETE || data[1] == BT_DATA_NAME_SHORTENED) {
			name = value;
			name_size = count;
		}
		data += length + 1;
		size -= length + 1;
	}
	if (type == BT_GAP_ADV_TYPE_ADV_IND) {
		if (xbox() ? !gamepad || !microsoft : !service || !kind) goto out;
		peer.address = *address;
		if (!xbox()) peer.state.lwp3.kind = selected_kind;
		peer.candidate = true;
	} else if (!peer.candidate || bt_addr_le_cmp(address, &peer.address)) goto out;
	if (!name) goto out;
	size_t filter = strlen(peer.config.name);
	if (name_size < filter || memcmp(name, peer.config.name, filter)) {
		peer.candidate = false;
		goto out;
	}
	if (!IS_ENABLED(CONFIG_BT_SMP) && needs_pairing()) {
		fail(-ENOTSUP);
		goto out;
	}
	name_size = MIN(name_size, xbox() ? BP_BLE_PEER_NAME_MAX : BP_LWP3_NAME_MAX);
	memcpy(peer.state.name, name, name_size);
	peer.state.name[name_size] = 0;
	memcpy(peer.config.name, peer.state.name, sizeof(peer.config.name));
	peer.phase = CREATE;
	peer.deadline = 0;
	wake();
out:
	k_spin_unlock(&lock, key);
}

/* Called under lock after enumerating or reading one HID Report. */
static void next_report(void)
{
	if (++peer.report_index < peer.report_count) peer.phase = HID_DESCRIPTOR;
	else if (!peer.value_handle || !peer.output_handle || !peer.ccc_handle) fail(-ENOTSUP);
	else peer.phase = SUBSCRIBE;
}

static uint8_t read_reference(struct bt_conn *conn, uint8_t error,
	struct bt_gatt_read_params *params, const void *data, uint16_t size)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint8_t result = BT_GATT_ITER_STOP;
	if (conn != peer.conn || peer.cancel || peer.phase != HID_REFERENCE) goto done;
	if (error || !data || !size || size > sizeof(peer.reference) - peer.reference_size) {
		fail(error ? -EIO : -EPROTO);
	} else {
		memcpy(peer.reference + peer.reference_size, data, size);
		peer.reference_size += size;
		if (peer.reference_size < sizeof(peer.reference)) {
			result = BT_GATT_ITER_CONTINUE;
			goto done;
		}
		struct hid_report *report = &peer.reports[peer.report_index];
		if (peer.reference[0] == 1 && peer.reference[1] == 1) {
			if (peer.value_handle || !report->ccc || !(report->properties & BT_GATT_CHRC_NOTIFY)) fail(-ENOTSUP);
			else {
				peer.value_handle = report->handle;
				peer.ccc_handle = report->ccc;
			}
		} else if (peer.reference[0] == 3 && peer.reference[1] == 2) {
			if (peer.output_handle || !(report->properties & BT_GATT_CHRC_WRITE)) fail(-ENOTSUP);
			else peer.output_handle = report->handle;
		}
		if (!peer.cancel) next_report();
	}
	peer.pending = false;
	peer.deadline = 0;
	wake();
done:
	k_spin_unlock(&lock, key);
	return result;
}

static uint8_t discovered(struct bt_conn *conn, const struct bt_gatt_attr *attr,
	struct bt_gatt_discover_params *params)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn != peer.conn || peer.cancel) goto done;
	peer.pending = false;
	if (peer.phase == CHARACTERISTIC && xbox()) {
		if (!attr) {
			if (!peer.report_count) fail(-ENOENT);
			else {
				struct hid_report *last = &peer.reports[peer.report_count - 1];
				if (!last->end) last->end = peer.service_end;
				peer.report_index = 0;
				peer.phase = HID_DESCRIPTOR;
			}
		} else {
			const struct bt_gatt_chrc *chrc = attr->user_data;
			if (peer.report_count && !peer.reports[peer.report_count - 1].end) {
				peer.reports[peer.report_count - 1].end = attr->handle - 1;
			}
			if (!bt_uuid_cmp(chrc->uuid, BT_UUID_HIDS_REPORT)) {
				if (peer.report_count == ARRAY_SIZE(peer.reports)) fail(-E2BIG);
				else if (chrc->value_handle <= attr->handle || chrc->value_handle >= peer.service_end) fail(-EPROTO);
				else peer.reports[peer.report_count++] = (struct hid_report){
					.handle = chrc->value_handle, .properties = chrc->properties};
			}
			if (!peer.cancel) {
				peer.pending = true;
				k_spin_unlock(&lock, key);
				return BT_GATT_ITER_CONTINUE;
			}
		}
	} else if (peer.phase == HID_DESCRIPTOR) {
		struct hid_report *report = &peer.reports[peer.report_index];
		if (!attr) {
			if (!report->reference) fail(-ENOENT);
			else {
				peer.phase = HID_REFERENCE;
				peer.reference_size = 0;
			}
		} else {
			if (!bt_uuid_cmp(attr->uuid, BT_UUID_HIDS_REPORT_REF)) report->reference = attr->handle;
			else if (!bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CCC)) report->ccc = attr->handle;
			peer.pending = true;
			k_spin_unlock(&lock, key);
			return BT_GATT_ITER_CONTINUE;
		}
	} else if (!attr) fail(-ENOENT);
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
			peer.output_handle = value->value_handle;
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
	if (!peer.handle) goto done;
	if (xbox()) {
		if (xbox_device_input(&peer.state.xbox, data, size, peer.config.xbox_deadzone, k_uptime_get())) {
			peer.state.xbox.error = -EPROTO;
			peer.state.xbox.valid = 0;
		}
		goto done;
	}
	const uint8_t *value = data;
	if (!peer.handle || size < 3 || size > BP_LWP3_MESSAGE_MAX || value[0] != size) goto done;
	if (peer.config.lwp3.profile != BP_LWP3_RAW) lwp3_device_notify(&peer.state.lwp3, data, size, k_uptime_get());
	if (peer.config.lwp3.notifications) {
		unsigned int index = (peer.first + peer.count) % peer.config.lwp3.notifications;
		peer.messages[index].size = size;
		memcpy(peer.messages[index].data, data, size);
		if (peer.count < peer.config.lwp3.notifications) peer.count++;
		else peer.first = (peer.first + 1) % peer.config.lwp3.notifications;
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
		if (!error && !xbox() && peer.tx.size > 5 && peer.tx.size <= 5 + BP_LWP3_NAME_MAX &&
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
		} else peer.phase = needs_pairing() ? SECURITY : SERVICE;
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

void hub_ble_peer_process(int radio_error, bool scan_stopped)
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
		int size = xbox() ? 0 : lwp3_device_setup(peer.config.lwp3.profile, peer.state.lwp3.kind, peer.setup_step, peer.tx.data);
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
		if (xbox() && bt_conn_get_security(conn) >= BT_SECURITY_L2) {
			/* Reusing a bond may have encrypted the link before our worker ran.
			 * set_security() would then return success without a new callback. */
			key = k_spin_lock(&lock);
			if (!peer.cancel && peer.phase == SECURITY) {
				peer.pending = false;
				peer.paired = true;
				peer.deadline = 0;
				peer.phase = SERVICE;
				wake();
			}
			k_spin_unlock(&lock, key);
		} else error = bt_conn_set_security(conn, BT_SECURITY_L2 | (xbox() ? 0 : BT_SECURITY_FORCE_PAIR));
#else
		error = -ENOTSUP;
#endif
		break;
	case SERVICE:
	case CHARACTERISTIC:
	case CCC:
	case HID_DESCRIPTOR:
		peer.discovery = (struct bt_gatt_discover_params){
			.uuid = phase == SERVICE ? (xbox() ? BT_UUID_HIDS : &service_uuid.uuid) :
				phase == CHARACTERISTIC ? (xbox() ? NULL : &characteristic_uuid.uuid) :
				phase == HID_DESCRIPTOR ? NULL : BT_UUID_GATT_CCC,
			.func = discovered,
			.start_handle = phase == SERVICE ? BT_ATT_FIRST_ATTRIBUTE_HANDLE :
				phase == CHARACTERISTIC ? peer.service_start + 1 :
				phase == HID_DESCRIPTOR ? peer.reports[peer.report_index].handle + 1 : peer.value_handle + 1,
			.end_handle = phase == SERVICE ? BT_ATT_LAST_ATTRIBUTE_HANDLE :
				phase == HID_DESCRIPTOR ? peer.reports[peer.report_index].end : peer.service_end,
			.type = phase == SERVICE ? BT_GATT_DISCOVER_PRIMARY :
				phase == CHARACTERISTIC ? BT_GATT_DISCOVER_CHARACTERISTIC : BT_GATT_DISCOVER_DESCRIPTOR,
		};
		error = bt_gatt_discover(conn, &peer.discovery);
		break;
	case HID_REFERENCE:
		peer.read = (struct bt_gatt_read_params){.func = read_reference, .handle_count = 1,
			.single = {.handle = peer.reports[peer.report_index].reference}};
		error = bt_gatt_read(conn, &peer.read);
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
			.handle = peer.output_handle, .data = peer.tx.data, .length = peer.tx.size};
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
		if (phase == CLOSING) host_radio_failure("peer disconnect", error);
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
