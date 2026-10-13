// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <zephyr/device.h>
#include <stddef.h>
#include <stdint.h>

struct cc2640_info {
	uint8_t address[6]; /* Bluetooth little-endian order, as in factory flash. */
	uint16_t revision; /* HCI revision; TI SDK version encoding on this chip. */
	uint16_t manufacturer;
	uint32_t unrelated_events; /* Non-completing events, including TI status ACKs. */
};

/* Initialize the factory TI network processor with its preserved address.
 * Does not start GAP, advertise, pair or modify controller flash. This
 * synchronous operation belongs on the controller worker, never the hub loop
 * or system workqueue. A failed start attempts reset without replacing its
 * first error; info is published only on success. No concurrent command/start
 * operation is permitted. */
int cc2640_controller_start(const struct device *npi, const uint8_t address[6],
			    struct cc2640_info *info);

struct cc2640_gatt;
/* Initialize peripheral GAP and register the management database once after a
 * successful start, preserving the reference service/GAP ordering. Publish all
 * handles only on success. Failure attempts reset; start must then be repeated.
 * Uses the same controller worker; does not advertise, pair or erase bonds. */
int cc2640_controller_peripheral(struct cc2640_gatt *gatt);

/* Runtime operations share the same worker. Unsolicited events, including ones
 * received while waiting for a command's completion, are delivered in order.
 * The callback must copy retained bytes and must not call controller I/O.
 * A negative callback result faults the operation. */
typedef int (*cc2640_event_handler_t)(const uint8_t *event, size_t size);
/* ATT resource exhaustion returns -EAGAIN without faulting the controller;
 * keep the entire command for retry. -ENOTCONN also does not fault it. */
int cc2640_controller_command(const uint8_t *packet, size_t size,
			      cc2640_event_handler_t receive);
/* Nonblocking receive: one means a delivered event, zero means no input. */
int cc2640_controller_poll(cc2640_event_handler_t receive);
/* Assert reset and invalidate the database; no flash or bond writes. */
int cc2640_controller_stop(void);
