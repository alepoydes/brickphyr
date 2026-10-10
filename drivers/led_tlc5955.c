// SPDX-License-Identifier: Apache-2.0

#define DT_DRV_COMPAT ti_tlc5955

#include <pbdrv/tlc5955.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/spi.h>

struct tlc_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec latch;
	struct pwm_dt_spec clock;
	const uint8_t (*channels)[3];
	size_t led_count;
	const uint8_t *full_channels;
	size_t full_count;
};

struct tlc_data {
	struct k_mutex lock;
	uint8_t frame[TLC5955_DATA_SIZE];
};

/* Same electrical settings as Pybricks: 3.2 mA, ES-PWM and auto repeat. */
static const TLC5955_CONTROL_DATA(control, 127, TLC5955_MC_3_2, 127, 1, 0, 0, 1, 1);

static int transfer(const struct tlc_config *cfg, const uint8_t *frame)
{
	const struct spi_buf buf = {.buf = (void *)frame, .len = TLC5955_DATA_SIZE};
	const struct spi_buf_set tx = {.buffers = &buf, .count = 1};
	int err = spi_write_dt(&cfg->spi, &tx);

	if (err) {
		return err;
	}
	err = gpio_pin_set_dt(&cfg->latch, 1);
	if (err) {
		return err;
	}
	/* LAT must not become back-to-back register writes below the minimum pulse. */
	k_busy_wait(1);
	return gpio_pin_set_dt(&cfg->latch, 0);
}

static int set_color(const struct device *dev, uint32_t led, uint8_t count,
		     const uint8_t *colors)
{
	const struct tlc_config *cfg = dev->config;
	struct tlc_data *data = dev->data;

	if (led >= cfg->led_count || count != 3) {
		return -EINVAL;
	}
	k_mutex_lock(&data->lock, K_FOREVER);
	for (int i = 0; i < 3; i++) {
		pbdrv_tlc5955_set_duty(data->frame, cfg->channels[led][i], colors[i] * 257U);
	}
	int err = transfer(cfg, data->frame);

	k_mutex_unlock(&data->lock);
	return err;
}

static int set_brightness(const struct device *dev, uint32_t led, uint8_t value)
{
	if (value > 100) {
		return -EINVAL;
	}
	uint8_t level = value * 255U / 100U;
	const uint8_t colors[] = {level, level, level};

	return set_color(dev, led, 3, colors);
}

static int tlc_init(const struct device *dev)
{
	const struct tlc_config *cfg = dev->config;
	struct tlc_data *data = dev->data;

	if (!spi_is_ready_dt(&cfg->spi) || !gpio_is_ready_dt(&cfg->latch) ||
	    !pwm_is_ready_dt(&cfg->clock)) {
		return -ENODEV;
	}
	k_mutex_init(&data->lock);
	int err = gpio_pin_configure_dt(&cfg->latch, GPIO_OUTPUT_INACTIVE);

	if (err) {
		return err;
	}
	/* Resolve the period to cycles, rounding instead of truncating 104 ns to 9 ticks. */
	uint64_t rate;

	err = pwm_get_cycles_per_sec(cfg->clock.dev, cfg->clock.channel, &rate);
	if (err) {
		return err;
	}
	uint32_t cycles = DIV_ROUND_CLOSEST(rate * cfg->clock.period, NSEC_PER_SEC);

	err = pwm_set_cycles(cfg->clock.dev, cfg->clock.channel, cycles, cycles / 2,
			     cfg->clock.flags);
	if (err) {
		return err;
	}
	/* The maximum-current setting needs two control latches. */
	for (int i = 0; i < 2; i++) {
		err = transfer(cfg, control);
		if (err) {
			return err;
		}
	}
	for (size_t i = 0; i < cfg->full_count; i++) {
		if (cfg->full_channels[i] >= TLC5955_NUM_CHANNEL) {
			return -EINVAL;
		}
		pbdrv_tlc5955_set_duty(data->frame, cfg->full_channels[i], UINT16_MAX);
	}
	return transfer(cfg, data->frame);
}

static DEVICE_API(led, tlc_api) = {
	.set_color = set_color,
	.set_brightness = set_brightness,
};

#define LED_CHANNELS(node) DT_PROP(node, channels),
#define CHECK_CHANNELS(node)                                                                       \
	BUILD_ASSERT(DT_PROP_LEN(node, channels) == 3);                                              \
	BUILD_ASSERT(DT_PROP_BY_IDX(node, channels, 0) < TLC5955_NUM_CHANNEL);                        \
	BUILD_ASSERT(DT_PROP_BY_IDX(node, channels, 1) < TLC5955_NUM_CHANNEL);                        \
	BUILD_ASSERT(DT_PROP_BY_IDX(node, channels, 2) < TLC5955_NUM_CHANNEL);
#define TLC_DEFINE(n)                                                                              \
	DT_INST_FOREACH_CHILD_STATUS_OKAY(n, CHECK_CHANNELS)                                         \
	static const uint8_t channels_##n[][3] = {                                                  \
		DT_INST_FOREACH_CHILD_STATUS_OKAY(n, LED_CHANNELS)};                                \
	static const uint8_t full_##n[] = DT_INST_PROP_OR(n, initial_full_channels, {});             \
	static const struct tlc_config config_##n = {                                              \
		.spi = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(8) | SPI_TRANSFER_MSB),               \
		.latch = GPIO_DT_SPEC_INST_GET(n, latch_gpios),                                     \
		.clock = PWM_DT_SPEC_INST_GET(n),                                                  \
		.channels = channels_##n, .led_count = ARRAY_SIZE(channels_##n),                    \
		.full_channels = full_##n, .full_count = ARRAY_SIZE(full_##n),                       \
	};                                                                                         \
	static struct tlc_data data_##n;                                                           \
	DEVICE_DT_INST_DEFINE(n, tlc_init, NULL, &data_##n, &config_##n, POST_KERNEL,                  \
			      CONFIG_LED_INIT_PRIORITY, &tlc_api);

DT_INST_FOREACH_STATUS_OKAY(TLC_DEFINE)
