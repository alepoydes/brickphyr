/* SPDX-License-Identifier: MIT */
#ifndef BRICKPHYR_CC2640_NPI_H
#define BRICKPHYR_CC2640_NPI_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#define CC2640_NPI_MAX_PACKET 255

/* Kernel-only transport for the factory TI network processor, not generic HCI.
 * One controller worker owns reset, command correlation and received events.
 * Concurrent operations return -EBUSY. Initialization leaves reset asserted. */
int cc2640_npi_reset(const struct device *dev);
int cc2640_npi_off(const struct device *dev);

/* Edge/error wake for the controller worker. The semaphore must have static
 * lifetime; NULL detaches it. Publishing a non-NULL semaphore gives it once
 * so a ready level that predates registration cannot strand the worker. */
void cc2640_npi_set_wake(const struct device *dev, struct k_sem *wake);

/* Exchange an optional HCI command (01, opcode LE, parameter length, parameters)
 * for an optional HCI event (04, event code, parameter length, parameters).
 * event must hold CC2640_NPI_MAX_PACKET bytes and may overlap command: TX is
 * consumed before a complete event is published. The result is its length or an
 * errno. With no command, timeout bounds waiting for unsolicited input;
 * -EAGAIN means no input and does not fault the transport. SPI/handshake/framing
 * faults assert reset and remain latched until an explicit successful reset.
 * Completion of transmission is not completion of the HCI command. */
int cc2640_npi_exchange(const struct device *dev, const uint8_t *command,
		       size_t length, uint8_t *event, k_timeout_t timeout);

#endif
