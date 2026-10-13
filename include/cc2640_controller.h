// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <zephyr/device.h>
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
