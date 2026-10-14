// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host_transport.h"
#include <sys/types.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>

#define HOST_BLE_SHELL_UUID(id) BT_UUID_128_ENCODE(0xf3641400 + (id), \
	0x00b0, 0x4240, 0xba50, 0x05ca45bf8abc)

/* Opaque, reference-counted backend connection. A retained object must never
 * refer to a later connection, even if the controller reuses its numeric ID.
 * Callbacks for a given link are serialized by its backend. */
struct host_ble_link;
enum host_ble_value { HOST_BLE_VALUE_PYBRICKS, HOST_BLE_VALUE_SHELL, HOST_BLE_VALUE_SMP };

/* Backend hooks. Ref/unref/MTU are nonblocking and valid under a spinlock.
 * Notify copies/queues bytes without waiting for controller replies; -EAGAIN
 * or -ENOMEM retains the caller's bytes for retry. Disconnect queues work. */
struct host_ble_link *host_ble_link_ref(struct host_ble_link *link);
void host_ble_link_unref(struct host_ble_link *link);
uint16_t host_ble_link_mtu(struct host_ble_link *link);
int host_ble_link_notify(struct host_ble_link *link, enum host_ble_value value,
			const uint8_t *data, size_t size);
void host_ble_link_disconnect(struct host_ble_link *link);

int host_ble_session_open(struct host_ble_link *link);
void host_ble_session_close(struct host_ble_link *link);
enum host_ble_status host_ble_session_profile(void);
/* GATT results are byte counts or negative ATT protocol errors. */
ssize_t host_ble_session_read(struct host_ble_link *link, bool caps, uint16_t id,
			     void *data, uint16_t size, uint16_t offset);
ssize_t host_ble_session_command(struct host_ble_link *link, const void *data,
				uint16_t size, uint16_t offset, bool prepared);
ssize_t host_ble_session_ccc_check(struct host_ble_link *link, uint16_t value);
void host_ble_session_subscribed(uint16_t value);
ssize_t host_ble_session_shell_subscribe(struct host_ble_link *link, uint16_t value);
ssize_t host_ble_session_shell_input(struct host_ble_link *link, const void *data,
				    uint16_t size, uint16_t offset, bool prepared);
bool host_ble_session_smp_begin(struct host_ble_link *link);
void host_ble_session_smp_end(void);
