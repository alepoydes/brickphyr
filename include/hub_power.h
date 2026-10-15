// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum hub_connection { HUB_DISCONNECTED, HUB_USB, HUB_BLE };
enum hub_power_phase { HUB_POWER_RUNNING, HUB_POWER_STOPPING, HUB_POWER_OFF, HUB_POWER_WAKING };

struct hub_power_state {
	int64_t sampled_ms;
	int64_t idle_since_ms;
	int source;
	uint32_t battery_idle_s;
	uint32_t usb_idle_s;
	bool connected; /* Management reservation, including disconnect cleanup. */
	enum hub_connection transport; /* Live accepted connection, excluding cleanup. */
	bool application;
	bool shutting_down;
};

/* A read-only policy snapshot; sampled_ms belongs to the last VBUS read. */
void hub_power_get(struct hub_power_state *state);

/* A host acquisition and expiry claim serialize on the same policy lock. */
bool hub_power_connection(enum hub_connection transport);
/* The link ended, while accepted work may still hold the power reservation. */
void hub_power_link_closed(enum hub_connection transport);
/* An autonomous application's lifetime holds power independently of a host. */
bool hub_power_application(bool running);
/* Explicit shutdown also closes admission, even with an active owner. */
void hub_power_request_shutdown(void);
/* The hub loop publishes OFF only after quiescing devices and releasing power.
 * Wake is idempotent while running/waking, and rejected during shutdown. */
void hub_power_mark_off(void);
enum hub_power_phase hub_power_phase_get(void);
int hub_power_request_wake(void);
/* VBUS: 0 absent, 1 present, negative errno unknown. True claims shutdown. */
bool hub_power_update(int vbus, bool activity, int64_t now);
/* Load known keys through the same runtime validation used by host commands. */
int hub_power_load(void);
