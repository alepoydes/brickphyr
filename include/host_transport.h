// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

enum host_mode { HOST_PYBRICKS, HOST_SHELL };

/* One shared stream/Shell; USB selection is permitted between closed sessions.
 * Owner commands can request the next mode, applied when their session closes. */
int host_transport_init(const struct device *uart, uint16_t hub_variant);
void host_connection_changed(bool open);
void host_interface_status(uint8_t state[8]);
enum host_mode host_interface_current(void);
int host_interface_select(uint16_t mode);

/* BLE claims the same management slot. Release only the successfully acquired
 * session. Disconnect defers release until its accepted SMP packet finishes. */
int host_ble_acquire(void);
void host_ble_release(bool exit_compatibility);
uint16_t host_hub_variant(void);
/* Terminal shutdown: stop asynchronously; no later radio enable until reboot. */
void host_ble_shutdown(void);
/* Link/profile observation only; USB selection does not select a BLE service.
 * A native terminal owns the profile until its stream closes. Otherwise the
 * last accepted profile-specific operation identifies the connection. */
enum host_ble_status {
	HOST_BLE_OFF, HOST_BLE_WAITING, HOST_BLE_CONNECTED,
	HOST_BLE_PYBRICKS, HOST_BLE_NATIVE,
};
enum host_ble_status host_ble_status(void);

/* Packet callbacks run on one processing thread; only that thread may perform
 * settings operations under the BLE grant, even while disconnect is draining. */
bool host_ble_request_begin(void);
void host_ble_request_end(void);
bool host_ble_request_active(void);
/* New commands stop at disconnect; an already entered handler may finish. */
bool host_ble_command_allowed(void);

/* Native BLE byte stream reuses the same Shell, buffers and session lifecycle.
 * The nonblocking callback schedules TX work. A session token rejects stale I/O.
 * RX writes are all-or-nothing; output is consumed only after successful send.
 * Close joins accepted Shell work asynchronously before releasing ownership. */
int host_ble_shell_open(void (*tx_ready)(void), uint32_t *session);
void host_ble_shell_close(uint32_t session);
int host_ble_shell_receive(uint32_t session, const void *data, size_t length);
size_t host_ble_shell_peek(uint32_t session, void *data, size_t capacity);
void host_ble_shell_consume(uint32_t session, size_t length);
