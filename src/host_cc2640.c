// SPDX-License-Identifier: Apache-2.0
/* CC2640 supplies GAP/GATT; Brickphyr supplies the existing session and Shell.
 * Only this worker performs controller I/O. Callbacks may queue work but never
 * wait for radio replies on the system queue or in the hub's power loop. */
#include "host_cc2640.h"
#include "host_cc2640_smp.h"
#include "cc2640_controller.h"
#include "cc2640_gatt.h"
#include "cc2640_npi.h"
#include "host_protocol.h"

#include <errno.h>
#include <string.h>
#include <pbio/protocol.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(host_cc2640, LOG_LEVEL_ERR);

#define NO_CONNECTION UINT16_MAX
/* Same bounded output-stall policy as the common BLE session. Retry temporary
 * chip backpressure at the shared Shell's 10 ms interval, without spinning. */
#define TX_TIMEOUT_MS 3000
#define TX_RETRY_MS 10
#define DISCONNECT_TIMEOUT_MS 2000

enum radio_state { RADIO_OFF, RADIO_STARTING, RADIO_ON, RADIO_STOPPING, RADIO_FAILED };
static atomic_t state, wanted, shutting_down;
static const struct device *transport;
static uint8_t factory_address[6];
static struct cc2640_gatt gatt;
static int first_error;
static const char *failed_operation;
static atomic_t ignored_events, busy_replies;
static K_SEM_DEFINE(wake, 0, 1);
static struct k_thread worker;
/* Inspected ARM paths use about 1.1 KiB before nested kernel/library calls.
 * Keep room for those calls and exception frames; physical high-water is open. */
static K_KERNEL_STACK_DEFINE(worker_stack, 2048);
static struct k_spinlock link_lock;
struct host_ble_link {
	atomic_t references;
	uint16_t connection;
	uint32_t epoch;
	bool active;
	atomic_t closing;
};
/* Do not reuse this object or advertise until all old references have gone. */
static struct host_ble_link link;
static bool advertising, disconnect_sent;
static int64_t disconnect_deadline;
static uint16_t rejected = NO_CONNECTION;
static bool rejection_sent;
static int64_t rejection_deadline;

struct transmission {
	uint8_t data[CC2640_GATT_PACKET_MAX];
	uint8_t size;
	uint32_t epoch;
	int64_t retry_at, expires;
	int8_t subscription; /* -1 for ATT response; otherwise the CCC array index. */
};
/* One ATT request may arrive after the previous response reaches the peer but
 * before its local command status. Keep that request separate from the reply
 * awaiting admission. Notifications have one copied mailbox plus one in flight. */
static struct transmission att_pending;
static struct {
	uint8_t data[CC2640_ATT_MTU - 3], size;
	uint32_t epoch;
	enum host_ble_value value;
	bool pending;
} notification;

static int subscription(enum host_ble_value value)
{
	return value == HOST_BLE_VALUE_PYBRICKS ? 0 : value == HOST_BLE_VALUE_SHELL ? 2 : 3;
}

static uint32_t output_generation(int ccc)
{
#ifdef CONFIG_BRICKPHYR_CC2640_SMP
	if (ccc == 3) return host_cc2640_smp_generation();
#endif
	return link.epoch;
}

struct host_ble_link *host_ble_link_ref(struct host_ble_link *peer)
{
	atomic_inc(&peer->references);
	return peer;
}

void host_ble_link_unref(struct host_ble_link *peer)
{
	if (atomic_dec(&peer->references) == 1) k_sem_give(&wake);
}

uint16_t host_ble_link_mtu(struct host_ble_link *peer)
{
	ARG_UNUSED(peer);
	return CC2640_ATT_MTU;
}

int host_ble_link_notify(struct host_ble_link *peer, enum host_ble_value value,
			const uint8_t *data, size_t size)
{
	if (!data || !size || size > sizeof(notification.data)) return -EMSGSIZE;
	if (value != HOST_BLE_VALUE_PYBRICKS && value != HOST_BLE_VALUE_SHELL &&
	    !(IS_ENABLED(CONFIG_BRICKPHYR_CC2640_SMP) && value == HOST_BLE_VALUE_SMP)) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&link_lock);
	int err = atomic_get(&state) == RADIO_FAILED ? -EIO :
		peer != &link || !link.active || atomic_get(&link.closing) ? -ENOTCONN :
		notification.pending ? -EAGAIN : 0;
	if (!err) {
		memcpy(notification.data, data, size);
		notification.size = size;
		notification.value = value;
		notification.epoch = output_generation(subscription(value));
		notification.pending = true;
		k_sem_give(&wake);
	}
	k_spin_unlock(&link_lock, key);
	return err;
}

void host_ble_link_disconnect(struct host_ble_link *peer)
{
	k_spinlock_key_t key = k_spin_lock(&link_lock);
	if (peer == &link && link.active) {
		atomic_set(&link.closing, 1);
		k_sem_give(&wake);
	}
	k_spin_unlock(&link_lock, key);
}

enum host_ble_status host_ble_status(void)
{
	return atomic_get(&state) == RADIO_ON ? host_ble_session_profile() : HOST_BLE_OFF;
}

/* Worker only; connection events and confirmed controller reset reach here. */
static void close_link(void)
{
	k_spinlock_key_t key = k_spin_lock(&link_lock);
	bool active = link.active;
	link.active = false;
	notification.pending = false;
	k_spin_unlock(&link_lock, key);
	att_pending.size = 0;
	memset(gatt.subscribed, 0, sizeof(gatt.subscribed));
	disconnect_sent = false;
	if (active) {
#ifdef CONFIG_BRICKPHYR_CC2640_SMP
		host_cc2640_smp_subscribe(&link, false);
#endif
		host_ble_session_close(&link);
		host_ble_link_unref(&link);
	}
}

static int receive_event(const uint8_t *event, size_t size)
{
	if (size < 3 || event[0] != 4 || event[2] != size - 3) return -EBADMSG;
	if (event[1] != 0xff) goto ignored;
	if (size < 6) return -EBADMSG;
	uint16_t code = sys_get_le16(event + 3);
	if (code == 0x0605) {
		/* Failed establishments need not contain a usable connection handle. */
		if (event[5]) goto ignored;
		if (size != 23) return -EBADMSG;
		uint16_t connection = sys_get_le16(event + 13);
		if (event[15] != 4 || connection == NO_CONNECTION) return -EPROTO;
		advertising = false;
		if (link.active || atomic_get(&link.references)) {
			if (rejected != NO_CONNECTION) return -ENOBUFS;
			rejected = connection;
			rejection_sent = false;
			return 0;
		}
		k_spinlock_key_t key = k_spin_lock(&link_lock);
		link.connection = connection;
		link.epoch++;
		if (!link.epoch) link.epoch++;
		atomic_set(&link.closing, !atomic_get(&wanted) || atomic_get(&shutting_down));
		link.active = true;
		atomic_set(&link.references, 1);
		k_spin_unlock(&link_lock, key);
		memset(gatt.subscribed, 0, sizeof(gatt.subscribed));
		if (!atomic_get(&link.closing) && !host_ble_session_open(&link)) return 0;
		host_ble_link_disconnect(&link);
		return 0;
	}
	if (code == 0x0606) {
		if (size != 9 || event[5]) return -EBADMSG;
		uint16_t connection = sys_get_le16(event + 6);
		if (link.active && link.connection == connection) close_link();
		if (rejected == connection) rejected = NO_CONNECTION;
		return 0;
	}
	if ((code & 0xff00) == 0x0500) {
		if (!link.active || atomic_get(&link.closing) || (code & 1) || code == 0x051e) goto ignored;
		/* Write commands need no response; a second request cannot overwrite
		 * an unsent reply or perform its command before we reject overflow. */
		if (att_pending.size && !(code == 0x0512 && size >= 13 && event[10] == 1)) {
			return -ENOBUFS;
		}
		uint8_t reply[CC2640_GATT_PACKET_MAX];
		int count = cc2640_gatt_request(&gatt, &link, link.connection, event, size, reply);
		if (count == -ENOTCONN || count == -ENOTSUP) goto ignored;
		if (count < 0) return count;
		if (count) {
			if (att_pending.size) return -ENOBUFS;
			memcpy(att_pending.data, reply, count);
			att_pending.size = count;
			att_pending.epoch = link.epoch;
			att_pending.retry_at = 0;
			att_pending.expires = k_uptime_get() + TX_TIMEOUT_MS;
			att_pending.subscription = -1;
		}
		return 0;
	}
ignored:
	atomic_inc(&ignored_events);
	return 0;
}

static int command(const uint8_t *data, size_t size)
{
	return cc2640_controller_command(data, size, receive_event);
}

static int prepare_advertising(void)
{
	/* Same legacy discovery layout as F4: Pybricks in advertising; SMP (or
	 * PnP without SMP) beside the name in scan response. */
	uint8_t packet[37] = {1, 7, 0xfe, 23, 1, 21, 2, 1, 6, 17, 6};
	sys_memcpy_swap(packet + 11, pbio_pybricks_service_uuid, 16);
	int err = command(packet, 27);
	if (err) return err;
	packet[4] = 0;
#ifdef CONFIG_BRICKPHYR_CC2640_SMP
	packet[6] = 17;
	packet[7] = 6;
	const uint8_t smp_uuid[] = {SMP_BT_SVC_UUID_VAL};
	memcpy(packet + 8, smp_uuid, sizeof(smp_uuid));
	const size_t prefix = 24;
#else
	packet[6] = 10;
	packet[7] = 0x16;
	sys_put_le16(PBIO_GATT_PNP_ID_CHAR_UUID, packet + 8);
	pbio_pybricks_pnp_id(packet + 10, host_protocol_hub_kind(), host_hub_variant());
	const size_t prefix = 17;
#endif
	uint8_t name[HOST_PROTOCOL_PAYLOAD_MAX - 3];
	size_t length = host_protocol_read(PBIO_PYBRICKS_READ_SERVICE_GATT,
		PBIO_GATT_DEVICE_NAME_CHAR_UUID, host_hub_variant(), 20, name);
	if (!length) return -EMSGSIZE;
	size_t count = MIN(length, sizeof(packet) - prefix - 2);
	packet[prefix] = count + 1;
	packet[prefix + 1] = count == length ? 9 : 8; /* complete/shortened name */
	memcpy(packet + prefix + 2, name, count);
	packet[5] = prefix - 6 + 2 + count;
	packet[3] = 2 + packet[5];
	return command(packet, packet[3] + 4);
}

static void fail_radio(const char *operation, int err)
{
	if (atomic_get(&state) != RADIO_FAILED) {
		first_error = err;
		failed_operation = operation;
		atomic_set(&state, RADIO_FAILED);
		LOG_ERR("%s: %d; reboot required", operation, err);
	}
	/* Only confirmed reset permits releasing an otherwise live connection.
	 * Cleanup must not replace the original error or retry indefinitely. */
	if (!cc2640_controller_stop()) close_link();
	advertising = false;
}

static bool current(const struct transmission *tx)
{
	return link.active && !atomic_get(&link.closing) && tx->epoch == output_generation(tx->subscription) &&
		(tx->subscription < 0 || gatt.subscribed[tx->subscription]);
}

static int transmit(struct transmission *tx)
{
	if (!current(tx)) { tx->size = 0; return 0; }
	int err = command(tx->data, tx->size);
	if (!current(tx)) { tx->size = 0; return 0; }
	if (err == -ENOTCONN) { close_link(); tx->size = 0; return 0; }
	if (err == -EAGAIN) {
		atomic_inc(&busy_replies);
		if (k_uptime_get() >= tx->expires) {
			host_ble_link_disconnect(&link);
			tx->size = 0;
			LOG_ERR("Output stalled after %u busy replies; closing link",
				(unsigned int)atomic_get(&busy_replies));
		} else tx->retry_at = k_uptime_get() + TX_RETRY_MS;
		return 0;
	}
	if (!err) tx->size = 0;
	return err;
}

static int terminate(uint16_t connection)
{
	uint8_t packet[] = {1, 0x0a, 0xfe, 3, 0, 0, 0x13};
	sys_put_le16(connection, packet + 4);
	return command(packet, sizeof(packet));
}

static void run(void *a, void *b, void *c)
{
	struct transmission att = {0}, output = {0};
	for (;;) {
		int status = atomic_get(&state);
		bool enable = atomic_get(&wanted) && !atomic_get(&shutting_down);
		if (status != RADIO_FAILED && enable != (status == RADIO_ON)) {
			atomic_set(&state, enable ? RADIO_STARTING : RADIO_STOPPING);
			int err;
			if (enable) {
				struct cc2640_info info;
				err = cc2640_controller_start(transport, factory_address, &info);
				if (!err) err = cc2640_controller_peripheral(&gatt);
				if (!err) err = prepare_advertising();
			} else {
				err = cc2640_controller_stop();
				if (!err) close_link();
				att.size = output.size = 0;
				rejected = NO_CONNECTION;
				advertising = false;
			}
			if (err) fail_radio(enable ? "initialization" : "shutdown", err);
			else atomic_set(&state, enable ? RADIO_ON : RADIO_OFF);
			continue;
		}
		if (status != RADIO_ON) { k_sem_take(&wake, K_FOREVER); continue; }
		int polled = cc2640_controller_poll(receive_event);
		if (polled < 0) { fail_radio("receive", polled); continue; }
		/* Shutdown can supersede a command or its received event. */
		if (!atomic_get(&wanted) || atomic_get(&shutting_down)) continue;
		int64_t now = k_uptime_get(), next = INT64_MAX;
		if (link.active && atomic_get(&link.closing) && !disconnect_sent) {
			disconnect_sent = true;
			disconnect_deadline = now + DISCONNECT_TIMEOUT_MS;
			int err = terminate(link.connection);
			if (err == -ENOTCONN) close_link();
			else if (err) fail_radio("disconnect", err);
			continue;
		}
		if (rejected != NO_CONNECTION && !rejection_sent) {
			rejection_sent = true;
			rejection_deadline = now + DISCONNECT_TIMEOUT_MS;
			int err = terminate(rejected);
			if (err == -ENOTCONN) rejected = NO_CONNECTION;
			else if (err) fail_radio("reject connection", err);
			continue;
		}
		if (disconnect_sent) next = MIN(next, disconnect_deadline);
		if (rejected != NO_CONNECTION) next = MIN(next, rejection_deadline);
		if (now >= next) { fail_radio("disconnect event", -ETIMEDOUT); continue; }
		if (!att.size && att_pending.size) { att = att_pending; att_pending.size = 0; }
		if (att.size && !current(&att)) att.size = 0;
		if (output.size && !current(&output)) output.size = 0;
		if (!output.size) {
			int encode_error = 0;
			k_spinlock_key_t key = k_spin_lock(&link_lock);
			if (notification.pending) {
				int ccc = subscription(notification.value);
				if (link.active && !atomic_get(&link.closing) && notification.epoch == output_generation(ccc)) {
					int count = cc2640_gatt_notification(&gatt, link.connection,
						notification.value, notification.data, notification.size, output.data);
					if (count > 0) {
						output.size = count;
						output.epoch = notification.epoch;
						output.retry_at = 0;
						output.expires = now + TX_TIMEOUT_MS;
						output.subscription = ccc;
					} else if (count != -ENOTCONN) encode_error = count;
				}
				notification.pending = false;
			}
			k_spin_unlock(&link_lock, key);
#ifdef CONFIG_BRICKPHYR_CC2640_SMP
			host_cc2640_smp_tx_ready();
#endif
			if (encode_error) { fail_radio("notification", encode_error); continue; }
		}
		/* ATT responses take precedence over further notifications. */
		struct transmission *tx = att.size ? &att : output.size ? &output : NULL;
		if (tx) {
			if (now >= tx->retry_at) {
				int err = transmit(tx);
				if (err) fail_radio("ATT transmit", err);
				continue;
			}
			next = MIN(next, tx->retry_at);
		}
		if (!advertising && !link.active && !atomic_get(&link.references) &&
		    rejected == NO_CONNECTION && host_ble_available(&wake)) {
			/* Publish first: a link event can precede MakeDiscoverableDone. */
			advertising = true;
			const uint8_t start[] = {1, 6, 0xfe, 10, 0, 2, 0, 0, 0, 0, 0, 0, 7, 0};
			int err = command(start, sizeof(start));
			if (err) fail_radio("advertising", err);
			continue;
		}
		if (polled) continue;
		k_sem_take(&wake, next == INT64_MAX ? K_FOREVER : K_MSEC(MAX(0, next - k_uptime_get())));
	}
}

void host_ble_shutdown(void)
{
	atomic_set(&shutting_down, 1);
	atomic_clear(&wanted);
	k_sem_give(&wake);
}

static int cmd_bluetooth(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 2) {
		bool enable = !strcmp(argv[1], "on");
		if (!enable && strcmp(argv[1], "off")) return -EINVAL;
		if (atomic_get(&shutting_down)) return -ESHUTDOWN;
		if (atomic_get(&state) == RADIO_FAILED) return first_error;
		atomic_set(&wanted, enable);
		k_sem_give(&wake);
		shell_print(shell, "Bluetooth %s requested.", enable ? "on" : "off");
	} else {
		const char *const names[] = {"off", "starting", "on", "stopping", "failed"};
		int status = atomic_get(&state);
		shell_print(shell, "Bluetooth: %s", names[status]);
		if (status == RADIO_FAILED) shell_print(shell, "%s failed: %d; reboot required",
			failed_operation, first_error);
		shell_print(shell, "Ignored events: %u; busy replies: %u",
			(unsigned int)atomic_get(&ignored_events), (unsigned int)atomic_get(&busy_replies));
	}
	return 0;
}
SHELL_CMD_ARG_REGISTER(bluetooth, NULL, "Bluetooth status, or on | off.", cmd_bluetooth, 1, 1);

int host_cc2640_init(const struct device *npi, const uint8_t address[6])
{
	if (!npi || !address || !device_is_ready(npi)) return -ENODEV;
	if (transport) return -EALREADY;
	int err = host_transport_init(NULL, 0);
	if (err) return err;
#ifdef CONFIG_BRICKPHYR_CC2640_SMP
	err = host_cc2640_smp_init();
	if (err) return err;
#endif
	transport = npi;
	memcpy(factory_address, address, sizeof(factory_address));
	cc2640_npi_set_wake(npi, &wake);
	atomic_set(&wanted, 1);
	k_thread_create(&worker, worker_stack, K_KERNEL_STACK_SIZEOF(worker_stack),
		run, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO - IS_ENABLED(CONFIG_BRICKPHYR_NATIVE), 0, K_NO_WAIT);
	k_thread_name_set(&worker, "cc2640");
	return 0;
}

#ifdef CONFIG_BOARD_TECHNIC_HUB
static int setup(void)
{
	/* Preserved loader identity, from the pinned Technic platform.c. */
	return host_cc2640_init(DEVICE_DT_GET(DT_NODELABEL(cc2640)), (const void *)0x08007ff0);
}
SYS_INIT(setup, APPLICATION, 90);
#endif
