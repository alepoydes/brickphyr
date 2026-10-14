// SPDX-License-Identifier: Apache-2.0
#include "cc2640_npi.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/ztest.h>

#define RADIO DEVICE_DT_GET(DT_NODELABEL(npi))
#define GPIO DEVICE_DT_GET(DT_NODELABEL(npi_gpio))
#define MRDY 0
#define SRDY 1
#define RESET 2

/* Complete, independent wire fixtures: event 04 0e 01 00 and command
 * 01 34 12 01 ab. RX has the controller's initial zero pad. */
static const uint8_t reply[] = {0, 0xfe, 4, 4, 0x0e, 1, 0, 0x0f};
static const uint8_t command[] = {1, 0x34, 0x12, 1, 0xab};
static const uint8_t command_wire[] = {0xfe, 5, 1, 0x34, 0x12, 1, 0xab, 0x88};
/* A write-only exchange may receive arbitrary bytes without an RX SOF. */
static const uint8_t empty_header[] = {0, 0x42, 0x17};

struct radio_fixture {
	const uint8_t *rx;
	size_t rx_size;
	const uint8_t *tx;
	size_t tx_size;
	int calls;
	int fail_call;
	bool auto_ready;
	bool reassert;
	bool exchanging;
	bool mismatch;
	int mrdy_releases;
};
static struct radio_fixture radio;
static struct gpio_callback wire;
static K_SEM_DEFINE(worker_wake, 0, 4);

static void on_mrdy(const struct device *port, struct gpio_callback *cb,
		    gpio_port_pins_t pins)
{
	ARG_UNUSED(cb);
	if (!(pins & BIT(MRDY))) return;
	int level = gpio_emul_output_get(port, MRDY);
	if (level == 0 && radio.auto_ready) {
		(void)gpio_emul_input_set(port, SRDY, 0);
	} else if (level == 1) {
		if (radio.exchanging) radio.mrdy_releases++;
		(void)gpio_emul_input_set(port, SRDY, 1);
		if (radio.exchanging && radio.reassert) {
			/* Both edges happen before gpio_pin_set_dt returns. */
			(void)gpio_emul_input_set(port, SRDY, 0);
		}
		radio.exchanging = false;
	}
}

static int on_spi(const struct emul *target, const struct spi_config *config,
		  const struct spi_buf_set *tx, const struct spi_buf_set *rx)
{
	ARG_UNUSED(target);
	ARG_UNUSED(config);
	if (rx->count != 1) return -EINVAL;
	size_t offset = radio.calls++ ? 3 : 0;
	size_t size = rx->buffers[0].len;
	size_t tx_size = 0;
	for (size_t segment = 0; segment < tx->count; segment++) {
		if (tx->buffers[segment].len > size - tx_size) return -EINVAL;
		tx_size += tx->buffers[segment].len;
	}
	if (tx_size != size) return -EINVAL;
	radio.exchanging = true;
	if (radio.calls == radio.fail_call) return -EIO;
	size_t rx_body = radio.rx_size > 3 ? radio.rx_size - 3 : 0;
	size_t tx_body = radio.tx_size > 3 ? radio.tx_size - 3 : 0;
	if (size != (offset ? MAX(rx_body, tx_body) : 3) ||
	    offset + size > CC2640_NPI_MAX_PACKET + 4) return -EINVAL;
	size_t clock = 0;
	for (size_t segment = 0; segment < tx->count; segment++) {
		const uint8_t *actual = tx->buffers[segment].buf;
		for (size_t i = 0; i < tx->buffers[segment].len; i++, clock++) {
			uint8_t want = offset + clock < radio.tx_size ? radio.tx[offset + clock] : 0;
			if ((actual ? actual[i] : 0) != want) radio.mismatch = true;
		}
	}
	uint8_t *out = rx->buffers[0].buf;
	for (size_t i = 0; i < size; i++) {
		out[i] = offset + i < radio.rx_size ? radio.rx[offset + i] : 0;
	}
	return radio.mismatch ? -EINVAL : 0;
}

static const struct spi_emul_api spi_api = {.io = on_spi};
static int emul_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);
	return 0;
}
EMUL_DT_DEFINE(DT_NODELABEL(npi), emul_init, NULL, NULL, &spi_api, NULL);

static void prepare(const uint8_t *rx, size_t rx_size,
		    const uint8_t *tx, size_t tx_size)
{
	radio.rx = rx;
	radio.rx_size = rx_size;
	radio.tx = tx;
	radio.tx_size = tx_size;
	radio.calls = 0;
	radio.fail_call = 0;
	radio.auto_ready = true;
	radio.reassert = true;
	radio.exchanging = false;
	radio.mismatch = false;
	radio.mrdy_releases = 0;
	(void)gpio_emul_input_set(GPIO, SRDY, 1);
}

static void *setup(void)
{
	zassert_true(device_is_ready(RADIO));
	zassert_true(device_is_ready(GPIO));
	gpio_init_callback(&wire, on_mrdy, BIT(MRDY));
	zassert_ok(gpio_add_callback(GPIO, &wire));
	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	prepare(reply, sizeof(reply), command_wire, sizeof(command_wire));
	zassert_ok(cc2640_npi_reset(RADIO));
}

ZTEST(cc2640_npi, test_full_duplex_and_fast_srdy_reassert)
{
	k_sem_reset(&worker_wake);
	/* SRDY was asserted before the worker subscribed. Registration must wake
	 * it without requiring another edge. */
	zassert_ok(gpio_emul_input_set(GPIO, SRDY, 0));
	cc2640_npi_set_wake(RADIO, &worker_wake);
	zassert_ok(k_sem_take(&worker_wake, K_NO_WAIT));
	zassert_equal(gpio_pin_get_raw(GPIO, SRDY), 0);
	zassert_ok(gpio_emul_input_set(GPIO, SRDY, 1));
	k_sem_reset(&worker_wake);
	uint8_t event[CC2640_NPI_MAX_PACKET] = {0};
	zassert_equal(cc2640_npi_exchange(RADIO, command, sizeof(command), event,
					   K_MSEC(1)), 4);
	zassert_mem_equal(event, reply + 3, 4);
	zassert_equal(radio.calls, 2);
	zassert_equal(radio.mrdy_releases, 1);
	zassert_false(radio.mismatch);
	/* The SRDY release was observed even though it is asserted again now. */
	zassert_equal(gpio_pin_get_raw(GPIO, SRDY), 0);
	zassert_true(k_sem_count_get(&worker_wake) >= 2);
	cc2640_npi_set_wake(RADIO, NULL);
	/* Once the event is consumed, its buffer may stage the next command. */
	prepare(reply, sizeof(reply), command_wire, sizeof(command_wire));
	memcpy(event, command, sizeof(command));
	zassert_equal(cc2640_npi_exchange(RADIO, event, sizeof(command), event,
					   K_MSEC(1)), 4);
	zassert_mem_equal(event, reply + 3, 4);
	zassert_equal(radio.calls, 2);
	zassert_false(radio.mismatch);
}

ZTEST(cc2640_npi, test_maximum_receive_and_transmit)
{
	uint8_t large_reply[3 + CC2640_NPI_MAX_PACKET + 1] = {0, 0xfe, 0xff,
		4, 0xff, 0xfc};
	large_reply[sizeof(large_reply) - 1] = 0xf8;
	uint8_t event[CC2640_NPI_MAX_PACKET] = {0};
	prepare(large_reply, sizeof(large_reply), NULL, 0);
	zassert_ok(gpio_emul_input_set(GPIO, SRDY, 0));
	zassert_equal(cc2640_npi_exchange(RADIO, NULL, 0, event, K_MSEC(1)),
		      CC2640_NPI_MAX_PACKET);
	zassert_mem_equal(event, large_reply + 3, sizeof(event));
	zassert_false(radio.mismatch);

	uint8_t large_command[CC2640_NPI_MAX_PACKET] = {1, 0x34, 0x12, 251};
	uint8_t large_wire[3 + CC2640_NPI_MAX_PACKET] = {0xfe, 0xff,
		1, 0x34, 0x12, 251};
	large_wire[sizeof(large_wire) - 1] = 0x23;
	prepare(empty_header, sizeof(empty_header), large_wire, sizeof(large_wire));
	zassert_equal(cc2640_npi_exchange(RADIO, large_command, sizeof(large_command),
					   event, K_MSEC(1)), 0);
	zassert_equal(radio.calls, 2);
	zassert_false(radio.mismatch);
}

ZTEST(cc2640_npi, test_bad_fcs_is_latched_without_publishing)
{
	uint8_t corrupt[sizeof(reply)];
	memcpy(corrupt, reply, sizeof(corrupt));
	corrupt[sizeof(corrupt) - 1] ^= 1;
	prepare(corrupt, sizeof(corrupt), NULL, 0);
	zassert_ok(gpio_emul_input_set(GPIO, SRDY, 0));
	uint8_t event[CC2640_NPI_MAX_PACKET];
	memset(event, 0xa5, sizeof(event));
	zassert_equal(cc2640_npi_exchange(RADIO, NULL, 0, event, K_MSEC(1)), -EBADMSG);
	zassert_equal(event[0], 0xa5);
	zassert_equal(gpio_emul_output_get(GPIO, RESET), 0);
	int calls = radio.calls;
	zassert_equal(cc2640_npi_exchange(RADIO, NULL, 0, event, K_NO_WAIT), -EBADMSG);
	zassert_equal(radio.calls, calls);
}

ZTEST(cc2640_npi, test_bad_read_header_never_clocks_a_body)
{
	static const uint8_t bad_header[] = {0, 0x12, 5};
	prepare(bad_header, sizeof(bad_header), NULL, 0);
	zassert_ok(gpio_emul_input_set(GPIO, SRDY, 0));
	uint8_t event[CC2640_NPI_MAX_PACKET] = {0xa5};
	zassert_equal(cc2640_npi_exchange(RADIO, NULL, 0, event, K_MSEC(1)), -EBADMSG);
	zassert_equal(radio.calls, 1);
	zassert_equal(event[0], 0xa5);
	zassert_equal(cc2640_npi_exchange(RADIO, NULL, 0, event, K_NO_WAIT), -EBADMSG);
	zassert_equal(radio.calls, 1);
}

ZTEST(cc2640_npi, test_handshake_timeout_requires_reset)
{
	radio.auto_ready = false;
	uint8_t event[CC2640_NPI_MAX_PACKET] = {0xa5};
	zassert_equal(cc2640_npi_exchange(RADIO, command, sizeof(command), event,
					   K_MSEC(1)), -ETIMEDOUT);
	zassert_equal(radio.calls, 0);
	zassert_equal(gpio_emul_output_get(GPIO, RESET), 0);
	zassert_equal(cc2640_npi_exchange(RADIO, command, sizeof(command), event,
					   K_NO_WAIT), -ETIMEDOUT);
	radio.auto_ready = true;
	zassert_ok(cc2640_npi_reset(RADIO));
	zassert_equal(cc2640_npi_exchange(RADIO, command, sizeof(command), event,
					   K_MSEC(1)), 4);
	zassert_mem_equal(event, reply + 3, 4);
}

ZTEST(cc2640_npi, test_spi_error_preserves_first_failure)
{
	radio.fail_call = 2;
	uint8_t event[CC2640_NPI_MAX_PACKET] = {0xa5};
	zassert_equal(cc2640_npi_exchange(RADIO, command, sizeof(command), event,
					   K_MSEC(1)), -EIO);
	zassert_equal(radio.calls, 2);
	zassert_equal(event[0], 0xa5);
	zassert_equal(gpio_emul_output_get(GPIO, RESET), 0);
	zassert_equal(cc2640_npi_exchange(RADIO, command, sizeof(command), event,
					   K_NO_WAIT), -EIO);
	zassert_equal(radio.calls, 2);
	radio.fail_call = 0;
	radio.calls = 0;
	zassert_ok(cc2640_npi_reset(RADIO));
	zassert_equal(cc2640_npi_exchange(RADIO, command, sizeof(command), event,
					   K_MSEC(1)), 4);
}

ZTEST_SUITE(cc2640_npi, NULL, setup, before, NULL, NULL);
