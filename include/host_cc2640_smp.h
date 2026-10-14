// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host_ble_session.h"

/* Only the controller worker changes subscriptions, feeds fragments or closes
 * them. Zephyr's SMP queue runs admitted commands independently of that worker. */
int host_cc2640_smp_init(void);
void host_cc2640_smp_subscribe(struct host_ble_link *link, bool enabled);
ssize_t host_cc2640_smp_receive(struct host_ble_link *link, const void *data, uint16_t size);
/* Nonblocking wake after mailbox retirement or link invalidation. */
void host_cc2640_smp_tx_ready(void);
/* Nonblocking generation check also invalidates copied backend output when a
 * client unsubscribes and subscribes again on the same physical connection. */
uint32_t host_cc2640_smp_generation(void);
