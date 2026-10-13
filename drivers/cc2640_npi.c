// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
/* SPI framing, handshake and reset sequence adapted from
 * pybricks-micropython a7b1b04ae4f8449dd46b3dfb97c420334439fd35,
 * lib/pbio/drv/bluetooth/bluetooth_stm32_cc2640.c.
 * Zephyr replaces the cooperative/DMA scheduling. Length/FCS validation,
 * bounded waits and first-error retention are Brickphyr adaptations.
 * See LICENSES/Pybricks-MIT.txt. No controller firmware is written here. */

#define DT_DRV_COMPAT ti_cc2640_npi

#include "cc2640_npi.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(cc2640_npi, LOG_LEVEL_ERR);

/* The reference allows 500 ms for the post-reset ready line. Use the same
 * failure bound for each handshake; this is not a measured transfer latency. */
#define HANDSHAKE_TIMEOUT K_MSEC(500)
#define NPI_SOF 0xfe
#define RX_HEADER_SIZE 3

struct npi_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec mrdy, srdy, reset;
};

struct npi_data {
	struct k_mutex lock;
	struct k_sem changed;
	atomic_ptr_t wake;
	struct gpio_callback callback;
	const struct device *dev;
	atomic_t releases;
	atomic_t gpio_error;
	int error;
	/* RX has a leading pad; both arrays also hold clocks beyond the shorter
	 * half of a full-duplex exchange. Never keep 255 + FCS in an 8-bit size. */
	uint8_t tx[RX_HEADER_SIZE + CC2640_NPI_MAX_PACKET + 1];
	uint8_t rx[RX_HEADER_SIZE + CC2640_NPI_MAX_PACKET + 1];
};

static void srdy_changed(const struct device *port, struct gpio_callback *cb,
			 gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	struct npi_data *data = CONTAINER_OF(cb, struct npi_data, callback);
	const struct npi_config *config = data->dev->config;
	int level = gpio_pin_get_dt(&config->srdy);
	if (level < 0) {
		atomic_cas(&data->gpio_error, 0, level);
	} else if (!level) {
		atomic_inc(&data->releases);
	}
	k_sem_give(&data->changed);
	struct k_sem *wake = atomic_ptr_get(&data->wake);
	if (wake) k_sem_give(wake);
}

void cc2640_npi_set_wake(const struct device *dev, struct k_sem *wake)
{
	struct npi_data *data = dev->data;
	atomic_ptr_set(&data->wake, wake);
	/* Wake the new owner even if SRDY was asserted before publication. */
	if (wake) k_sem_give(wake);
}

static int hold_reset(const struct device *dev)
{
	const struct npi_config *config = dev->config;
	int err = gpio_pin_configure_dt(&config->reset, GPIO_OUTPUT_ACTIVE);
	int release_err = gpio_pin_set_dt(&config->mrdy, 0);
	return err ? err : release_err;
}

static int fail(const struct device *dev, const char *operation, int err)
{
	struct npi_data *data = dev->data;
	if (!data->error || data->error == -EHOSTDOWN) {
		data->error = err;
		LOG_ERR("%s: %d", operation, err);
	}
	/* Even if cleanup fails, retain the operation's first error. */
	(void)hold_reset(dev);
	return data->error;
}

static int wait_srdy(const struct device *dev, bool active, k_timeout_t timeout)
{
	const struct npi_config *config = dev->config;
	struct npi_data *data = dev->data;
	k_timepoint_t deadline = sys_timepoint_calc(timeout);
	for (;;) {
		int err = atomic_get(&data->gpio_error);
		if (err) return err;
		int level = gpio_pin_get_dt(&config->srdy);
		if (level < 0) return level;
		if (level == active) return 0;
		if (sys_timepoint_expired(deadline) ||
		    k_sem_take(&data->changed, sys_timepoint_timeout(deadline))) {
			return -EAGAIN;
		}
	}
}

static int release_mrdy(const struct device *dev)
{
	const struct npi_config *config = dev->config;
	struct npi_data *data = dev->data;
	atomic_val_t releases = atomic_get(&data->releases);
	int err = gpio_pin_set_dt(&config->mrdy, 0);
	if (err) return err;
	k_timepoint_t deadline = sys_timepoint_calc(HANDSHAKE_TIMEOUT);
	/* The controller may release and immediately reassert SRDY for its next
	 * event. A level-only wait would lose that acknowledgment. */
	while (atomic_get(&data->releases) == releases) {
		err = atomic_get(&data->gpio_error);
		if (err) return err;
		if (sys_timepoint_expired(deadline) ||
		    k_sem_take(&data->changed, sys_timepoint_timeout(deadline))) {
			return -ETIMEDOUT;
		}
	}
	return atomic_get(&data->gpio_error);
}

int cc2640_npi_reset(const struct device *dev)
{
	const struct npi_config *config = dev->config;
	struct npi_data *data = dev->data;
	if (k_mutex_lock(&data->lock, K_NO_WAIT)) return -EBUSY;
	int err = hold_reset(dev);
	if (err) goto done;
	atomic_clear(&data->gpio_error);
	k_sem_reset(&data->changed);
	k_sleep(K_MSEC(150));
	err = gpio_pin_configure_dt(&config->reset, GPIO_OUTPUT_INACTIVE);
	if (err) goto done;
	k_sleep(K_MSEC(150));
	/* Preserve the reference's high-then-input sequence; its electrical
	 * reason has not been established on this board. No pull is enabled. */
	err = gpio_pin_configure_dt(&config->reset, GPIO_INPUT);
	if (err) goto done;
	k_sleep(K_MSEC(100));
	err = wait_srdy(dev, false, HANDSHAKE_TIMEOUT);
	if (err == -EAGAIN) err = -ETIMEDOUT;
done:
	if (err) err = fail(dev, "reset", err);
	else data->error = 0;
	k_mutex_unlock(&data->lock);
	return err;
}

int cc2640_npi_off(const struct device *dev)
{
	struct npi_data *data = dev->data;
	if (k_mutex_lock(&data->lock, K_NO_WAIT)) return -EBUSY;
	int err = hold_reset(dev);
	if (err) err = fail(dev, "off", err);
	else if (!data->error) data->error = -EHOSTDOWN;
	k_mutex_unlock(&data->lock);
	return err;
}

static int transfer(const struct device *dev, size_t offset, size_t size)
{
	const struct npi_config *config = dev->config;
	struct npi_data *data = dev->data;
	struct spi_buf tx = {.buf = data->tx + offset, .len = size};
	struct spi_buf rx = {.buf = data->rx + offset, .len = size};
	return spi_transceive_dt(&config->spi, &(struct spi_buf_set){&tx, 1},
				&(struct spi_buf_set){&rx, 1});
}

int cc2640_npi_exchange(const struct device *dev, const uint8_t *command,
		       size_t length, uint8_t *event, k_timeout_t timeout)
{
	if (!event || (length && (!command || length < 4 ||
	    length > CC2640_NPI_MAX_PACKET || command[0] != 1 ||
	    command[3] != length - 4))) return -EINVAL;
	const struct npi_config *config = dev->config;
	struct npi_data *data = dev->data;
	if (k_mutex_lock(&data->lock, K_NO_WAIT)) return -EBUSY;
	int err = data->error;
	const char *operation = "receive ready";
	if (err) goto done;
	if (!length) {
		err = wait_srdy(dev, true, timeout);
		if (err == -EAGAIN) goto done;
		if (err) goto fault;
	}

	memset(data->tx, 0, sizeof(data->tx));
	size_t tx_size = length ? length + 3 : 0;
	if (length) {
		data->tx[0] = NPI_SOF;
		data->tx[1] = length;
		memcpy(data->tx + 2, command, length);
		uint8_t fcs = 0;
		for (size_t i = 1; i < tx_size - 1; i++) fcs ^= data->tx[i];
		data->tx[tx_size - 1] = fcs;
	}
	operation = "assert MRDY";
	err = gpio_pin_set_dt(&config->mrdy, 1);
	if (err) goto fault;
	operation = "assert SRDY";
	err = wait_srdy(dev, true, HANDSHAKE_TIMEOUT);
	if (err == -EAGAIN) err = -ETIMEDOUT;
	if (err) goto fault;
	operation = "SPI header";
	err = transfer(dev, 0, RX_HEADER_SIZE);
	if (err) goto fault;

	size_t rx_size = 0;
	operation = "NPI header";
	if (data->rx[1] == NPI_SOF) {
		rx_size = data->rx[2];
		if (rx_size < 3) {
			err = -EBADMSG;
			goto fault;
		}
	} else if (!length) {
		/* The reference treats a non-SOF header during a write as no RX;
		 * it does not specify idle byte values. A read-only malformed header
		 * must never underflow tx_size - 3. */
		err = -EBADMSG;
		goto fault;
	}
	size_t remaining = MAX(rx_size ? rx_size + 1 : 0,
			       tx_size ? tx_size - RX_HEADER_SIZE : 0);
	operation = "SPI body";
	err = transfer(dev, RX_HEADER_SIZE, remaining);
	if (err) goto fault;
	if (rx_size) {
		operation = "event length/FCS";
		uint8_t fcs = 0;
		for (size_t i = 2; i < RX_HEADER_SIZE + rx_size + 1; i++) {
			fcs ^= data->rx[i];
		}
		if (fcs || data->rx[3] != 4 || data->rx[5] != rx_size - 3) {
			err = -EBADMSG;
			goto fault;
		}
	}
	operation = "release handshake";
	err = release_mrdy(dev);
	if (err) goto fault;
	/* Publish only a complete frame after the entire handshake succeeds. */
	memcpy(event, data->rx + RX_HEADER_SIZE, rx_size);
	err = rx_size;
	goto done;
fault:
	err = fail(dev, operation, err);
done:
	k_mutex_unlock(&data->lock);
	return err;
}

static int npi_init(const struct device *dev)
{
	const struct npi_config *config = dev->config;
	struct npi_data *data = dev->data;
	data->dev = dev;
	data->error = -EHOSTDOWN;
	k_mutex_init(&data->lock);
	k_sem_init(&data->changed, 0, 1);
	if (!spi_is_ready_dt(&config->spi) || !gpio_is_ready_dt(&config->mrdy) ||
	    !gpio_is_ready_dt(&config->srdy) || !gpio_is_ready_dt(&config->reset)) {
		return -ENODEV;
	}
	int err = gpio_pin_configure_dt(&config->reset, GPIO_OUTPUT_ACTIVE);
	if (err) return err;
	err = gpio_pin_configure_dt(&config->mrdy, GPIO_OUTPUT_INACTIVE);
	if (err) return err;
	err = gpio_pin_configure_dt(&config->srdy, GPIO_INPUT);
	if (err) return err;
	gpio_init_callback(&data->callback, srdy_changed, BIT(config->srdy.pin));
	err = gpio_add_callback(config->srdy.port, &data->callback);
	if (err) return err;
	err = gpio_pin_interrupt_configure_dt(&config->srdy, GPIO_INT_EDGE_BOTH);
	if (err) gpio_remove_callback(config->srdy.port, &data->callback);
	return err;
}

#define NPI_DEFINE(inst) \
	static const struct npi_config config_##inst = { \
		.spi = SPI_DT_SPEC_INST_GET(inst, SPI_WORD_SET(8), 0), \
		.mrdy = GPIO_DT_SPEC_INST_GET(inst, mrdy_gpios), \
		.srdy = GPIO_DT_SPEC_INST_GET(inst, srdy_gpios), \
		.reset = GPIO_DT_SPEC_INST_GET(inst, reset_gpios), \
	}; \
	static struct npi_data data_##inst; \
	DEVICE_DT_INST_DEFINE(inst, npi_init, NULL, &data_##inst, &config_##inst, \
			      POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(NPI_DEFINE)
