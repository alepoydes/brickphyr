// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Pybricks Authors
/* Star routing, per-source inboxes and relay framing adapted from
 * pybricks/messaging/pb_type_hub_network.c and
 * lib/pbio/drv/bluetooth/bluetooth_btstack.c at a7b1b04ae.
 * Zephyr owns BR/EDR and L2CAP. Queues belong to the kernel, never the VM. */
#include "hub_network.h"
#include "host_radio.h"
#include <limits.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/classic/classic.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/sys/byteorder.h>

#define NETWORK_PSM 0x1001
#define HEADER_SIZE 7
#define NETWORK_MTU (HEADER_SIZE + BP_NETWORK_DATA_MAX)
/* Allow the controller's page window plus Zephyr's channel setup window. */
#define CONNECT_TIMEOUT_MS (DIV_ROUND_UP(CONFIG_BT_PAGE_TIMEOUT * 5U, 8U) + \
	CONFIG_BT_L2CAP_CONN_RTX_TIMEOUT * 1000U)
#define SEND_TIMEOUT_MS 1000

BUILD_ASSERT(CONFIG_BT_BUF_ACL_RX_SIZE >= NETWORK_MTU + 4);
BUILD_ASSERT(CONFIG_BT_MAX_CONN >= BP_NETWORK_PEERS + 1);

enum phase { FREE, INCOMING, CREATE, PAGING, CHANNEL, READY, CLOSING };
struct peer {
	struct bt_l2cap_br_chan channel;
	struct bt_conn *conn;
	uint8_t address[6];
	enum phase phase;
	bool outgoing, pending, channel_live, ended, acl_ready;
	int error;
	int64_t deadline;
};
struct ring {
	uint8_t *data;
	uint32_t tail, used, count;
};
struct inbox {
	struct ring ring;
	uint8_t address[6];
	bool assigned;
};
static const uint8_t all[6] = {255, 255, 255, 255, 255, 255};
static struct k_spinlock lock;
static struct peer peers[BP_NETWORK_PEERS];
static struct inbox inboxes[BP_NETWORK_PEERS];
static struct ring relay;
static uint32_t capacity, lease, lease_sequence, message_sequence;
static struct bp_network_state state;
static enum { NONE, CONNECT, SEND } operation;
static uint8_t target[6], pending_mask;
static struct bp_network_message local_tx, relay_tx;
static uint8_t relay_mask;
static int64_t send_deadline;
static bool registered;
static const struct bt_l2cap_chan_ops channel_ops;

static void wake(void) { host_radio_schedule(K_NO_WAIT); }
static bool equal(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 6); }
static int owner(uint32_t handle) { return handle && handle == lease ? 0 : -ESTALE; }

static void tx_destroy(struct net_buf *buf)
{
	net_buf_destroy(buf);
	wake();
}
NET_BUF_POOL_FIXED_DEFINE(tx_pool, BP_NETWORK_PEERS, BT_L2CAP_BUF_SIZE(NETWORK_MTU),
	CONFIG_BT_CONN_TX_USER_DATA_SIZE, tx_destroy);

/* Both reference rings wrap byte offsets; copy contiguous spans in this port. */
static void ring_read(const struct ring *r, uint32_t offset, void *data, uint32_t size)
{
	uint32_t first = MIN(size, capacity - offset);
	memcpy(data, r->data + offset, first);
	memcpy((uint8_t *)data + first, r->data, size - first);
}
static void ring_write(struct ring *r, uint32_t offset, const void *data, uint32_t size)
{
	uint32_t first = MIN(size, capacity - offset);
	memcpy(r->data + offset, data, first);
	memcpy(r->data, (const uint8_t *)data + first, size - first);
}
static void discard(struct ring *r, uint32_t size)
{
	r->tail = (r->tail + size) % capacity;
	r->used -= size;
	r->count--;
}
static void append(struct ring *r, const uint8_t *header, uint32_t header_size,
		   const uint8_t *data, uint32_t size)
{
	uint32_t offset = (r->tail + r->used) % capacity;
	ring_write(r, offset, header, header_size);
	ring_write(r, (offset + header_size) % capacity, data, size);
	r->used += header_size + size;
	r->count++;
}
static void inbox_drop(struct inbox *box)
{
	uint8_t header[6];
	ring_read(&box->ring, box->ring.tail, header, sizeof(header));
	discard(&box->ring, sizeof(header) + sys_get_le16(header));
}
static void receive(const uint8_t *address, const uint8_t *data, uint32_t size)
{
	if (!lease) return;
	struct inbox *box = NULL;
	for (size_t i = 0; i < ARRAY_SIZE(inboxes); i++) {
		if (inboxes[i].assigned && equal(inboxes[i].address, address)) {
			box = &inboxes[i];
			break;
		}
		if (!inboxes[i].assigned && !box) box = &inboxes[i];
	}
	if (!box) { state.dropped++; return; }
	box->assigned = true;
	memcpy(box->address, address, 6);
	uint8_t header[6];
	sys_put_le16(size, header);
	sys_put_le32(message_sequence++, header + 2);
	while (box->ring.used + sizeof(header) + size > capacity) {
		inbox_drop(box);
		state.dropped++;
	}
	append(&box->ring, header, sizeof(header), data, size);
}
static void relay_put(uint8_t mask, const uint8_t *address, const uint8_t *data, uint32_t size)
{
	if (!mask || !relay.data) return;
	uint8_t header[12];
	while (relay.used + sizeof(header) + size > capacity) {
		ring_read(&relay, relay.tail, header, sizeof(header));
		discard(&relay, sizeof(header) + sys_get_le16(header + 4));
		state.dropped++;
	}
	sys_put_le32(mask, header);
	sys_put_le16(size, header + 4);
	memcpy(header + 6, address, 6);
	append(&relay, header, sizeof(header), data, size);
	wake();
}
static struct peer *by_address(const uint8_t *address)
{
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
		if (peers[i].phase != FREE && equal(peers[i].address, address)) return &peers[i];
	}
	return NULL;
}
static struct peer *vacant(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) if (peers[i].phase == FREE) return &peers[i];
	return NULL;
}
static bool live(bool outgoing)
{
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
		if (peers[i].phase != FREE && peers[i].outgoing == outgoing) return true;
	}
	return false;
}
static void complete(int error)
{
	state.error = error;
	state.busy = 0;
	operation = NONE;
	pending_mask = 0;
}
static void fail(struct peer *p, int error)
{
	if (!p->error) p->error = error;
	if (operation == CONNECT && equal(target, p->address)) complete(p->error);
	if (p->phase == CLOSING) return;
	/* Queued masks name slots. Remove this destination before its slot can
	 * be reused for a different brick, including frames waiting for relay. */
	uint8_t mask = BIT(p - peers);
	pending_mask &= ~mask;
	relay_mask &= ~mask;
	uint32_t offset = relay.tail;
	for (uint32_t i = 0; i < relay.count; i++) {
		uint8_t header[12];
		ring_read(&relay, offset, header, sizeof(header));
		header[0] &= ~mask;
		ring_write(&relay, offset, header, sizeof(header));
		offset = (offset + sizeof(header) + sys_get_le16(header + 4)) % capacity;
	}
	p->phase = CLOSING;
	p->pending = false;
	p->deadline = 0;
	wake();
}
static int request(void)
{
	if (++state.request > INT32_MAX || !state.request) state.request = 1;
	state.error = 0;
	state.busy = 1;
	return state.request;
}

static void channel_connected(struct bt_l2cap_chan *chan)
{
	struct peer *p = CONTAINER_OF(CONTAINER_OF(chan, struct bt_l2cap_br_chan, chan),
		struct peer, channel);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (p->phase != CLOSING) {
		if (p->channel.tx.mtu < NETWORK_MTU) fail(p, -EMSGSIZE);
		else {
			p->phase = READY;
			p->pending = false;
			p->deadline = 0;
			if (operation == CONNECT && equal(target, p->address)) complete(0);
		}
	}
	wake();
	k_spin_unlock(&lock, key);
}
static void channel_released(struct bt_l2cap_chan *chan)
{
	struct peer *p = CONTAINER_OF(CONTAINER_OF(chan, struct bt_l2cap_br_chan, chan),
		struct peer, channel);
	k_spinlock_key_t key = k_spin_lock(&lock);
	p->channel_live = false;
	if (p->phase != FREE) fail(p, -ECONNRESET);
	wake();
	k_spin_unlock(&lock, key);
}
static int channel_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	struct peer *p = CONTAINER_OF(CONTAINER_OF(chan, struct bt_l2cap_br_chan, chan),
		struct peer, channel);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (p->phase != READY || !lease) goto done;
	if (buf->len < HEADER_SIZE || buf->len > NETWORK_MTU || buf->data[0]) {
		state.malformed++;
		goto done;
	}
	const uint8_t *address = buf->data + 1, *data = buf->data + HEADER_SIZE;
	uint32_t size = buf->len - HEADER_SIZE;
	if (!p->outgoing) {
		receive(address, data, size);
		goto done;
	}
	bool broadcast = equal(address, all), local = equal(address, state.address);
	if (broadcast || local) receive(p->address, data, size);
	if (local && !broadcast) goto done;
	uint8_t mask = 0;
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
		if (&peers[i] != p && peers[i].phase == READY &&
		    (broadcast || equal(address, peers[i].address))) mask |= BIT(i);
	}
	relay_put(mask, p->address, data, size);
done:
	k_spin_unlock(&lock, key);
	return 0;
}
static const struct bt_l2cap_chan_ops channel_ops = {
	.connected = channel_connected, .released = channel_released, .recv = channel_recv,
};

static struct peer *connection_peer(struct bt_conn *conn)
{
	struct bt_conn_info info;
	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_BR) return NULL;
	uint8_t address[6];
	sys_memcpy_swap(address, info.br.dst->val, sizeof(address));
	struct peer *p = by_address(address);
	return p && (!p->conn || p->conn == conn) ? p : NULL;
}
static void connected(struct bt_conn *conn, uint8_t error)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct peer *p = connection_peer(conn);
	if (p) {
		if (!p->conn) p->conn = bt_conn_ref(conn);
		if (error) {
			p->ended = true;
			fail(p, -ECONNREFUSED);
		} else {
			p->acl_ready = true;
			if (p->phase != CLOSING) p->phase = CHANNEL;
			p->pending = false;
		}
		wake();
	}
	k_spin_unlock(&lock, key);
}
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct peer *p = connection_peer(conn);
	if (p) {
		p->ended = true;
		fail(p, -ECONNRESET);
		wake();
	}
	k_spin_unlock(&lock, key);
}
BT_CONN_CB_DEFINE(network_callbacks) = {.connected = connected, .disconnected = disconnected};

static enum bt_br_conn_req_rsp incoming(const bt_addr_t *address, uint32_t cod)
{
	ARG_UNUSED(cod);
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct peer *p = vacant();
	bool accept = !host_radio_error() && p && !live(true) && !live(false) && operation != CONNECT;
	if (accept) {
		/* Zephyr allocates this connection before invoking admission. Retain
		 * it even if the accept command fails without a connection callback. */
		p->conn = bt_conn_lookup_addr_br(address);
		accept = p->conn != NULL;
	}
	if (accept) {
		sys_memcpy_swap(p->address, address->val, 6);
		p->phase = INCOMING;
		p->deadline = k_uptime_get() + CONNECT_TIMEOUT_MS;
		host_radio_schedule(K_MSEC(CONNECT_TIMEOUT_MS));
	}
	k_spin_unlock(&lock, key);
	return accept ? BT_BR_CONN_REQ_ACCEPT_PERIPHERAL : BT_BR_CONN_REQ_REJECT_NO_RESOURCES;
}
static int accept_channel(struct bt_conn *conn, struct bt_l2cap_server *server,
			  struct bt_l2cap_chan **channel)
{
	ARG_UNUSED(server);
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct peer *p = connection_peer(conn);
	int error = -EACCES;
	if (p && !p->outgoing && p->phase == CHANNEL && !p->channel_live) {
		p->channel = (struct bt_l2cap_br_chan){.chan.ops = &channel_ops,
			.rx.mtu = NETWORK_MTU};
		p->channel_live = true;
		*channel = &p->channel.chan;
		error = 0;
	}
	k_spin_unlock(&lock, key);
	return error;
}
static struct bt_l2cap_server server = {
	.psm = NETWORK_PSM, .sec_level = BT_SECURITY_L2, .accept = accept_channel,
};

int hub_network_start(void)
{
	if (!registered) {
		int error = bt_l2cap_br_server_register(&server);
		if (error) return error;
		registered = true;
	}
	struct bt_br_oob oob;
	int error = bt_br_oob_get_local(&oob);
	if (error) return error;
	k_spinlock_key_t key = k_spin_lock(&lock);
	sys_memcpy_swap(state.address, oob.addr.val, 6);
	k_spin_unlock(&lock, key);
	return bt_br_set_connectable(true, incoming);
}

int hub_network_open(uint32_t size, uint32_t *handle)
{
	if (size < BP_NETWORK_INBOX_MIN || size > INT32_MAX / BP_NETWORK_PEERS) return -EINVAL;
	int error = host_radio_error();
	if (error) return host_radio_starting() ? -EAGAIN : error;
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (lease) error = -EBUSY;
	else {
		/* Empty ring metadata hides unused storage. Do not zero user-sized
		 * buffers here while the spinlock masks the port UART interrupts. */
		uint8_t *data = k_malloc(BP_NETWORK_PEERS * size);
		if (!data) error = -ENOMEM;
		else if (live(true) && !(relay.data = k_malloc(size))) {
			k_free(data);
			error = -ENOMEM;
		}
		else {
			capacity = size;
			memset(inboxes, 0, sizeof(inboxes));
			for (size_t i = 0; i < ARRAY_SIZE(inboxes); i++) inboxes[i].ring.data = data + i * size;
			if (!++lease_sequence) ++lease_sequence;
			*handle = lease = lease_sequence;
			state.error = state.dropped = state.malformed = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return error;
}
int hub_network_get(uint32_t handle, struct bp_network_state *value)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = owner(handle);
	if (!error) {
		*value = state;
		value->count = 0;
		memset(value->peers, 0, sizeof(value->peers));
		for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
			if (peers[i].phase == READY) memcpy(value->peers[value->count++], peers[i].address, 6);
		}
	}
	k_spin_unlock(&lock, key);
	return error;
}
int hub_network_connect(uint32_t handle, const uint8_t *address)
{
	int error = host_radio_error();
	if (error) return error;
	k_spinlock_key_t key = k_spin_lock(&lock);
	error = owner(handle);
	if (!error && (equal(address, all) || equal(address, state.address))) error = -EINVAL;
	if (!error && (operation != NONE || live(false))) error = -EBUSY;
	if (!error && !relay.data) {
		relay.data = k_malloc(capacity);
		if (!relay.data) error = -ENOMEM;
	}
	struct peer *p = by_address(address);
	if (!error && p && p->phase != READY) error = -EAGAIN;
	if (!error && !p) {
		p = vacant();
		if (!p) error = -ENOSPC;
		else {
			memcpy(p->address, address, 6);
			p->outgoing = true;
			p->phase = CREATE;
			p->deadline = k_uptime_get() + CONNECT_TIMEOUT_MS;
		}
	}
	if (!error) {
		error = request();
		memcpy(target, address, 6);
		operation = CONNECT;
		if (p->phase == READY) complete(0);
		else wake();
	}
	k_spin_unlock(&lock, key);
	return error;
}
int hub_network_send(uint32_t handle, const struct bp_network_message *message)
{
	if (message->size > BP_NETWORK_DATA_MAX) return -EMSGSIZE;
	int error = host_radio_error();
	if (error) return error;
	k_spinlock_key_t key = k_spin_lock(&lock);
	error = owner(handle);
	if (!error && operation != NONE) error = -EBUSY;
	if (!error) {
		local_tx = *message;
		pending_mask = 0;
		for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
			if (peers[i].phase != READY) continue;
			if (!peers[i].outgoing) { pending_mask = BIT(i); break; }
			if (equal(message->address, all) || equal(message->address, peers[i].address)) pending_mask |= BIT(i);
		}
		if (!live(false)) memcpy(local_tx.address, state.address, 6);
		error = request();
		operation = SEND;
		send_deadline = k_uptime_get() + SEND_TIMEOUT_MS;
		if (!pending_mask) complete(0);
		else wake();
	}
	k_spin_unlock(&lock, key);
	return error;
}
static void cancel(void)
{
	if (operation == CONNECT) {
		struct peer *p = by_address(target);
		if (p && p->phase != READY) fail(p, -ECANCELED);
	}
	if (operation != NONE) complete(-ECANCELED);
}
int hub_network_cancel(uint32_t handle, uint32_t token)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = owner(handle);
	if (!error && token != state.request) error = -ESTALE;
	if (!error) cancel();
	k_spin_unlock(&lock, key);
	return error;
}
int hub_network_snapshot(uint32_t handle, uint32_t latest, uint32_t *through)
{
	if (latest > 1) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = owner(handle);
	if (!error) {
		if (latest) {
			for (size_t i = 0; i < ARRAY_SIZE(inboxes); i++) {
				while (inboxes[i].ring.count > 1) inbox_drop(&inboxes[i]);
			}
		}
		*through = message_sequence;
	}
	k_spin_unlock(&lock, key);
	return error;
}
int hub_network_read(uint32_t handle, uint32_t through, struct bp_network_message *message)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = owner(handle);
	struct inbox *oldest = NULL;
	uint32_t sequence = 0, size = 0;
	for (size_t i = 0; !error && i < ARRAY_SIZE(inboxes); i++) {
		struct inbox *box = &inboxes[i];
		if (!box->ring.count) continue;
		uint8_t header[6];
		ring_read(&box->ring, box->ring.tail, header, sizeof(header));
		uint32_t candidate = sys_get_le32(header + 2);
		if ((int32_t)(candidate - through) >= 0) continue;
		if (!oldest || (int32_t)(candidate - sequence) < 0) {
			oldest = box;
			sequence = candidate;
			size = sys_get_le16(header);
		}
	}
	if (oldest) {
		memset(message, 0, sizeof(*message));
		memcpy(message->address, oldest->address, 6);
		message->size = size;
		ring_read(&oldest->ring, (oldest->ring.tail + 6) % capacity, message->data, size);
		discard(&oldest->ring, 6 + size);
		error = 1;
	}
	k_spin_unlock(&lock, key);
	return error;
}
static void release(void)
{
	cancel();
	lease = 0;
	k_free(inboxes[0].ring.data);
	memset(inboxes, 0, sizeof(inboxes));
	k_free(relay.data);
	relay = (struct ring){0};
	relay_mask = 0;
	capacity = 0;
}
int hub_network_close(uint32_t handle)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int error = owner(handle);
	if (!error) release();
	k_spin_unlock(&lock, key);
	return error;
}
int hub_network_release(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	release();
	k_spin_unlock(&lock, key);
	return 0;
}

/* The radio queue owns calls that may submit/wait for controller commands.
 * Peer/channel storage is static; a callback may retire a link while a call is
 * in flight, but only this worker can reuse its slot and release its conn ref. */
void hub_network_process(int radio_error)
{
	int64_t deadline = 0;
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
		struct peer *p = &peers[i];
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool expired = p->deadline && k_uptime_get() >= p->deadline;
		if (p->phase == INCOMING && (radio_error || expired)) {
			struct bt_conn_info info;
			if (p->conn && !bt_conn_get_info(p->conn, &info) &&
			    info.state == BT_CONN_STATE_DISCONNECTED) p->ended = true;
		}
		if (p->phase != FREE && radio_error && !p->ended) fail(p, radio_error);
		if (expired) fail(p, -ETIMEDOUT);
		struct bt_conn *retired = NULL;
		if ((p->ended || (p->phase == CLOSING && !p->conn)) && !p->channel_live) {
			retired = p->conn;
			memset(p, 0, sizeof(*p));
		}
		if (p->deadline && (!deadline || p->deadline < deadline)) deadline = p->deadline;
		enum phase phase = p->phase;
		bool issue = !radio_error && !p->pending && (phase == CREATE ||
			(phase == CHANNEL && p->outgoing && !p->channel_live) ||
			(phase == CLOSING && p->conn && !p->ended && (p->outgoing || p->acl_ready)));
		struct bt_conn *conn = p->conn;
		if (issue) {
			p->pending = true;
			if (phase == CREATE) p->phase = PAGING;
			if (phase == CHANNEL) {
				p->channel = (struct bt_l2cap_br_chan){.chan.ops = &channel_ops,
					.rx.mtu = NETWORK_MTU, .required_sec_level = BT_SECURITY_L2};
				p->channel_live = true;
			}
		}
		k_spin_unlock(&lock, key);
		if (retired) bt_conn_unref(retired);
		if (!issue) continue;
		int error = 0;
		if (phase == CREATE) {
			bt_addr_t address;
			sys_memcpy_swap(address.val, p->address, 6);
			struct bt_conn *created = bt_conn_create_br(&address, BT_BR_CONN_PARAM_DEFAULT);
			key = k_spin_lock(&lock);
			if (!created) { error = -ECONNREFUSED; p->ended = true; }
			else if (!p->conn) { p->conn = created; created = NULL; }
			k_spin_unlock(&lock, key);
			if (created) bt_conn_unref(created);
		} else if (phase == CHANNEL) error = bt_l2cap_chan_connect(conn, &p->channel.chan, NETWORK_PSM);
		else host_radio_disconnect(conn);
		if (error) {
			key = k_spin_lock(&lock);
			if (phase == CHANNEL && !p->channel.chan.conn) p->channel_live = false;
			fail(p, error);
			k_spin_unlock(&lock, key);
		}
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (radio_error && operation != NONE) complete(radio_error);
	if (operation == SEND && k_uptime_get() >= send_deadline) { state.dropped++; complete(0); }
	if (operation == SEND && (!deadline || send_deadline < deadline)) deadline = send_deadline;
	if (!relay_mask && relay.count) {
		uint8_t header[12];
		ring_read(&relay, relay.tail, header, sizeof(header));
		relay_mask = sys_get_le32(header);
		memcpy(relay_tx.address, header + 6, 6);
		relay_tx.size = sys_get_le16(header + 4);
		ring_read(&relay, (relay.tail + sizeof(header)) % capacity, relay_tx.data, relay_tx.size);
		discard(&relay, sizeof(header) + relay_tx.size);
	}
	bool relaying = relay_mask != 0;
	uint8_t *mask = relaying ? &relay_mask : &pending_mask;
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) if (peers[i].phase != READY) *mask &= ~BIT(i);
	struct net_buf *buf = NULL;
	struct peer *destination = NULL;
	uint32_t token = state.request;
	uint32_t sending_lease = lease;
	if (!radio_error && lease && *mask) {
		buf = net_buf_alloc(&tx_pool, K_NO_WAIT);
		if (buf) {
			for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
				if (*mask & BIT(i)) { destination = &peers[i]; *mask &= ~BIT(i); break; }
			}
			const struct bp_network_message *message = relaying ? &relay_tx : &local_tx;
			net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
			net_buf_add_u8(buf, 0);
			net_buf_add_mem(buf, message->address, 6);
			net_buf_add_mem(buf, message->data, message->size);
		}
	}
	if (!buf && operation == SEND && !pending_mask) complete(0);
	bool relay_done = relaying && !relay_mask;
	k_spin_unlock(&lock, key);
	if (buf) {
		int error = bt_l2cap_chan_send(&destination->channel.chan, buf);
		if (error < 0) net_buf_unref(buf);
		key = k_spin_lock(&lock);
		if (error < 0 && sending_lease == lease) state.dropped++;
		if (sending_lease == lease && !relaying && token == state.request && operation == SEND) {
			if (error < 0 && error != -ENOTCONN && error != -ESHUTDOWN) complete(error);
			else if (!pending_mask) complete(0);
		}
		k_spin_unlock(&lock, key);
		wake();
	} else if (relay_done) wake();
	if (deadline) host_radio_schedule(K_MSEC(MAX(1, deadline - k_uptime_get())));
}
