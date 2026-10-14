// SPDX-License-Identifier: Apache-2.0
/* CC2640 binding for Zephyr's SMP queue, parser and fragment reassembly.
 * Each packet owns a link reference; a retained link never changes identity. */
#include "host_cc2640_smp.h"

#include <errno.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp_reassembly.h>

struct packet_owner { struct host_ble_link *link; uint32_t session; };
BUILD_ASSERT(sizeof(struct packet_owner) <= CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE);
static struct smp_transport transport;
static struct k_spinlock lock;
static struct host_ble_link *subscriber;
static atomic_t session;
static K_SEM_DEFINE(tx_ready, 0, 1);

/* Caller holds lock. A new subscription on the same link is a new session. */
static bool current(const struct packet_owner *owner)
{
	return owner->link && owner->link == subscriber && owner->session == atomic_get(&session);
}

static void free_owner(void *data)
{
	struct packet_owner *owner = data;
	if (owner->link) host_ble_link_unref(owner->link);
	owner->link = NULL;
}

static int copy_owner(struct net_buf *dst, const struct net_buf *src)
{
	const struct packet_owner *from = net_buf_user_data(src);
	struct packet_owner *to = net_buf_user_data(dst);
	to->link = from->link ? host_ble_link_ref(from->link) : NULL;
	to->session = from->session;
	return 0;
}

static int begin(const struct net_buf *request, void **context)
{
	const struct packet_owner *owner = net_buf_user_data(request);
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool accepted = current(owner) && host_ble_session_smp_begin(owner->link);
	if (accepted) *context = host_ble_link_ref(owner->link);
	k_spin_unlock(&lock, key);
	return accepted ? MGMT_ERR_EOK : MGMT_ERR_EACCESSDENIED;
}

static void end(void *context, int result)
{
	ARG_UNUSED(result);
	host_ble_session_smp_end();
	host_ble_link_unref(context);
}

static uint16_t mtu(const struct net_buf *packet)
{
	const struct packet_owner *owner = net_buf_user_data(packet);
	return owner->link ? host_ble_link_mtu(owner->link) - 3 : 0;
}

static int output(struct net_buf *packet)
{
	const struct packet_owner *owner = net_buf_user_data(packet);
	int result = MGMT_ERR_EOK;
	int64_t deadline = k_uptime_get() + 3000;
	size_t offset = 0;
	while (offset < packet->len) {
		size_t count = MIN(packet->len - offset, mtu(packet));
		k_spinlock_key_t key = k_spin_lock(&lock);
		int err = !current(owner) ? -ENOTCONN :
			host_ble_link_notify(owner->link, HOST_BLE_VALUE_SMP, packet->data + offset, count);
		k_spin_unlock(&lock, key);
		if (!err) {
			offset += count;
			deadline = k_uptime_get() + 3000;
			continue;
		}
		if (err != -EAGAIN && err != -ENOMEM) {
			result = err == -ENOTCONN ? MGMT_ERR_ENOENT : MGMT_ERR_EUNKNOWN;
			break;
		}
		int64_t remaining = deadline - k_uptime_get();
		if (remaining <= 0) {
			host_ble_link_disconnect(owner->link);
			result = MGMT_ERR_ETIMEOUT;
			break;
		}
		/* The controller worker gives this after consuming its copied mailbox;
		 * a disconnect/unsubscribe wakes us too. Never occupy its RX thread. */
		k_sem_take(&tx_ready, K_MSEC(remaining));
	}
	free_owner(net_buf_user_data(packet));
	smp_packet_free(packet);
	return result;
}

int host_cc2640_smp_init(void)
{
	transport.functions = (struct smp_transport_api_t) {
		.output = output, .get_mtu = mtu, .ud_copy = copy_owner, .ud_free = free_owner,
		.request_begin = begin, .request_end = end,
	};
	return smp_transport_init(&transport);
}

void host_cc2640_smp_tx_ready(void)
{
	k_sem_give(&tx_ready);
}

uint32_t host_cc2640_smp_generation(void)
{
	return atomic_get(&session);
}

static void drop_partial(void)
{
	void *owner = smp_reassembly_get_ud(&transport);
	if (owner) free_owner(owner);
	smp_reassembly_drop(&transport);
}

void host_cc2640_smp_subscribe(struct host_ble_link *link, bool enabled)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct host_ble_link *next = enabled ? link : NULL;
	bool changed = subscriber != next;
	subscriber = next;
	if (changed) atomic_inc(&session);
	k_spin_unlock(&lock, key);
	if (changed || !enabled) {
		host_cc2640_smp_tx_ready();
		drop_partial();
		smp_rx_clear(&transport);
	}
}

ssize_t host_cc2640_smp_receive(struct host_ble_link *link, const void *data, uint16_t size)
{
	/* Subscription and input callbacks are serialized on the controller worker. */
	if (link != subscriber) return -BT_ATT_ERR_AUTHORIZATION;
	if (!size) return -BT_ATT_ERR_INVALID_ATTRIBUTE_LEN;
	bool first = smp_reassembly_expected(&transport) < 0;
	int remaining = smp_reassembly_collect(&transport, data, size);
	if (remaining < 0) {
		drop_partial();
		return remaining == -ENOMEM ? -BT_ATT_ERR_INSUFFICIENT_RESOURCES :
			-BT_ATT_ERR_INVALID_ATTRIBUTE_LEN;
	}
	if (first) {
		struct packet_owner *owner = smp_reassembly_get_ud(&transport);
		owner->link = host_ble_link_ref(link);
		owner->session = atomic_get(&session);
	}
	if (!remaining) smp_reassembly_complete(&transport, false);
	return size;
}
