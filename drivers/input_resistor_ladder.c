// SPDX-License-Identifier: Apache-2.0

#define DT_DRV_COMPAT lego_resistor_ladder

#include <pbdrv/resistor_ladder.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include "input_resistor_ladder.h"

LOG_MODULE_REGISTER(hub_keys);

struct ladder_config {
	struct adc_dt_spec adc;
	uint16_t thresholds[8];
	uint16_t codes[3];
	uint32_t period_ms;
};

struct ladder_data {
	struct k_work_delayable work;
	const struct device *dev;
	uint8_t previous;
	uint8_t reported;
	struct k_spinlock lock;
	struct resistor_ladder_sample last;
};

struct resistor_ladder_sample resistor_ladder_get_sample(const struct device *dev)
{
	struct ladder_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	struct resistor_ladder_sample last = data->last;

	k_spin_unlock(&data->lock, key);
	return last;
}

static void sample(struct k_work *work)
{
	struct ladder_data *data = CONTAINER_OF(k_work_delayable_from_work(work),
					       struct ladder_data, work);
	const struct ladder_config *cfg = data->dev->config;
	int16_t raw = -1;
	struct adc_sequence seq = {.buffer = &raw, .buffer_size = sizeof(raw)};
	pbdrv_resistor_ladder_ch_flags_t flags = 0;
	int err = adc_sequence_init_dt(&cfg->adc, &seq);

	if (!err) {
		err = adc_read_dt(&cfg->adc, &seq);
	}
	if (!err && pbdrv_resistor_ladder_decode(raw, cfg->thresholds, &flags) != PBIO_SUCCESS) {
		err = -EIO;
	}
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	data->last.raw = raw;
	data->last.error = err;
	if (err) {
		if (data->last.failures < UINT32_MAX) {
			data->last.failures++;
		}
		if (is_power_of_two(data->last.failures)) {
			LOG_ERR("%s: ADC %d, error %d, failures %u",
				data->dev->name, raw, err, data->last.failures);
		}
		/* Never keep a shutdown button pressed across an unreadable sample. */
		flags = 0;
		data->previous = 0;
	}

	k_spin_unlock(&data->lock, key);

	/* Two matching samples reject contact bounce; startup waits for settling. */
	if (flags == data->previous) {
		for (int i = 0; i < 3; i++) {
			if (cfg->codes[i] && ((flags ^ data->reported) & BIT(i))) {
				input_report_key(data->dev, cfg->codes[i], !!(flags & BIT(i)),
						 true, K_NO_WAIT);
			}
		}
		data->reported = flags;
	}
	data->previous = flags;
	k_work_schedule(&data->work, K_MSEC(cfg->period_ms));
}

static int ladder_init(const struct device *dev)
{
	const struct ladder_config *cfg = dev->config;
	struct ladder_data *data = dev->data;

	if (!adc_is_ready_dt(&cfg->adc)) {
		return -ENODEV;
	}
	int err = adc_channel_setup_dt(&cfg->adc);

	if (err) {
		return err;
	}
	data->dev = dev;
	data->last.error = -EAGAIN;
	k_work_init_delayable(&data->work, sample);
	/* Pybricks observes tens of milliseconds of invalid ladder voltage at boot. */
	k_work_schedule(&data->work, K_MSEC(50));
	return 0;
}

#define LADDER_DEFINE(n)                                                                            \
	BUILD_ASSERT(DT_INST_PROP_LEN(n, thresholds) == 8);                                          \
	BUILD_ASSERT(DT_INST_PROP_LEN(n, key_codes) == 3);                                           \
	static const struct ladder_config config_##n = {                                           \
		.adc = ADC_DT_SPEC_INST_GET(n),                                                     \
		.thresholds = DT_INST_PROP(n, thresholds),                                          \
		.codes = DT_INST_PROP(n, key_codes),                                                \
		.period_ms = DT_INST_PROP(n, sample_period_ms),                                     \
	};                                                                                         \
	static struct ladder_data data_##n;                                                        \
	DEVICE_DT_INST_DEFINE(n, ladder_init, NULL, &data_##n, &config_##n, POST_KERNEL,              \
			      CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(LADDER_DEFINE)
