// SPDX-License-Identifier: MIT
// Copyright (c) 2020-2026 The Pybricks Authors
/* Port of the Prime F4 DAC/timer path in sound_stm32_hal_dac.c.
 * DMA, clocks, pin control and GPIO use Zephyr. See ../src/sound/README.md. */
#define DT_DRV_COMPAT lego_prime_f4_speaker

#include "sound_output.h"
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <stm32_ll_dac.h>
#include <stm32_ll_tim.h>

struct config {
	DAC_TypeDef *dac;
	TIM_TypeDef *timer;
	struct stm32_pclken clocks[3];
	struct reset_dt_spec resets[2];
	const struct pinctrl_dev_config *pins;
	struct gpio_dt_spec enable;
	const struct device *dma;
	uint32_t channel, slot;
	void (*irq_setup)(void);
};

struct data {
	uint32_t clock, rate;
	size_t count;
	bool active;
	unsigned int next_half;
	int64_t last_callback_us;
	sound_output_callback callback;
	void *context;
};

/* IRQ-safe on this board: the amplifier uses an on-chip STM32 GPIO. DMA is
 * joined separately in stop; silencing cannot wait for a workqueue first. */
static int quiet(const struct device *dev)
{
	const struct config *cfg = dev->config;
	struct data *data = dev->data;
	LL_TIM_DisableCounter(cfg->timer);
	LL_DAC_DisableIT_DMAUDR1(cfg->dac);
	LL_DAC_DisableDMAReq(cfg->dac, LL_DAC_CHANNEL_1);
	LL_DAC_DisableTrigger(cfg->dac, LL_DAC_CHANNEL_1);
	data->active = false;
	return gpio_pin_set_dt(&cfg->enable, 0);
}

static void fail(const struct device *dev, int error)
{
	struct data *data = dev->data;
	if (data->active) {
		(void)quiet(dev);
		data->callback(data->context, error);
	}
}

static void underrun(const void *argument)
{
	const struct device *dev = argument;
	const struct config *cfg = dev->config;
	if (LL_DAC_IsActiveFlag_DMAUDR1(cfg->dac)) {
		LL_DAC_ClearFlag_DMAUDR1(cfg->dac);
		fail(dev, -EPIPE);
	}
}

static bool half_available(const struct device *dev, unsigned int half)
{
	const struct config *cfg = dev->config;
	struct data *data = dev->data;
	struct dma_status status;
	if (dma_get_status(cfg->dma, cfg->channel, &status) || !status.busy) {
		return false;
	}
	/* STM32 reports remaining transfers, not bytes. Half zero can be
	 * rewritten only while DMA reads half one, and vice versa. */
	return status.pending_length && status.pending_length <= data->count &&
		((status.pending_length > data->count / 2) == (half == 1));
}

static void transferred(const struct device *dma, void *argument, uint32_t channel, int status)
{
	const struct device *dev = argument;
	struct data *data = dev->data;
	if (!data->active) {
		return;
	}
	if (status < 0) {
		fail(dev, status);
		return;
	}
	/* This pinned STM32 driver reports HT as BLOCK and TC as COMPLETE. */
	unsigned int half = status == DMA_STATUS_BLOCK ? 0 : 1;
	int64_t now = k_cyc_to_us_floor64(k_cycle_get_64());
	if ((status != DMA_STATUS_BLOCK && status != DMA_STATUS_COMPLETE) ||
	    half != data->next_half || !half_available(dev, half) ||
	    now - data->last_callback_us >= (uint64_t)data->count * 1000000 / data->rate) {
		fail(dev, -EPIPE);
		return;
	}
	data->last_callback_us = now;
	data->next_half ^= 1;
	data->callback(data->context, half);
	/* Detect a refill that exceeded its half-buffer deadline. */
	if (data->active && !half_available(dev, half)) {
		fail(dev, -EPIPE);
	}
}

static int stop(const struct device *dev)
{
	const struct config *cfg = dev->config;
	unsigned int key = irq_lock();
	int error = quiet(dev);
	LL_DAC_Disable(cfg->dac, LL_DAC_CHANNEL_1);
	LL_DAC_ClearFlag_DMAUDR1(cfg->dac);
	irq_unlock(key);
	int dma_error = dma_stop(cfg->dma, cfg->channel);
	return error ? error : dma_error;
}

static int start(const struct device *dev, uint16_t *samples, size_t count, uint32_t rate,
	sound_output_callback callback, void *context, uint32_t *actual_rate)
{
	const struct config *cfg = dev->config;
	struct data *data = dev->data;
	if (!samples || ((uintptr_t)samples & 1) || count < 2 || count > UINT16_MAX ||
	    (count & 1) || rate < 8000 || rate > 48000 || !callback || !actual_rate) {
		return -EINVAL;
	}
	if (data->active) {
		return -EBUSY;
	}
	/* The F4 timer source includes APB's timer multiplier. Nearest divider;
	 * report the effective integer Hz instead of claiming every rate is exact. */
	uint32_t divider = DIV_ROUND_CLOSEST(data->clock, rate);
	if (!divider || divider > 65536) {
		return -ERANGE;
	}
	int err = stop(dev);
	if (err) {
		return err;
	}
	struct dma_block_config block = {
		.source_address = (uintptr_t)samples,
		.dest_address = (uintptr_t)&cfg->dac->DHR12L1,
		.block_size = count * sizeof(*samples),
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.source_reload_en = true, .dest_reload_en = true,
	};
	struct dma_config transfer = {
		.channel_direction = MEMORY_TO_PERIPHERAL, .channel_priority = 2,
		.source_data_size = 2, .dest_data_size = 2,
		.source_burst_length = 1, .dest_burst_length = 1,
		.dma_slot = cfg->slot, .block_count = 1, .head_block = &block,
		.dma_callback = transferred, .user_data = (void *)dev,
	};
	err = dma_config(cfg->dma, cfg->channel, &transfer);
	if (err) {
		return err;
	}
	LL_TIM_SetPrescaler(cfg->timer, 0);
	LL_TIM_SetAutoReload(cfg->timer, divider - 1);
	LL_TIM_SetCounter(cfg->timer, 0);
	LL_TIM_SetTriggerOutput(cfg->timer, LL_TIM_TRGO_UPDATE);
	LL_TIM_GenerateEvent_UPDATE(cfg->timer);
	LL_DAC_SetOutputBuffer(cfg->dac, LL_DAC_CHANNEL_1, LL_DAC_OUTPUT_BUFFER_ENABLE);
	LL_DAC_SetTriggerSource(cfg->dac, LL_DAC_CHANNEL_1, LL_DAC_TRIG_EXT_TIM6_TRGO);
	LL_DAC_ConvertData12LeftAligned(cfg->dac, LL_DAC_CHANNEL_1, 0x8000);
	LL_DAC_ClearFlag_DMAUDR1(cfg->dac);
	data->callback = callback;
	data->context = context;
	data->count = count;
	data->rate = data->clock / divider;
	data->next_half = 0;
	err = dma_start(cfg->dma, cfg->channel);
	if (!err) {
		LL_DAC_Enable(cfg->dac, LL_DAC_CHANNEL_1);
		/* ST specifies a DAC wake-up delay before the first external trigger. */
		k_busy_wait(LL_DAC_DELAY_STARTUP_VOLTAGE_SETTLING_US);
		err = gpio_pin_set_dt(&cfg->enable, 1);
	}
	if (err) {
		(void)stop(dev);
		return err;
	}
	unsigned int key = irq_lock();
	data->last_callback_us = k_cyc_to_us_floor64(k_cycle_get_64());
	data->active = true;
	LL_DAC_EnableIT_DMAUDR1(cfg->dac);
	LL_DAC_EnableDMAReq(cfg->dac, LL_DAC_CHANNEL_1);
	LL_DAC_EnableTrigger(cfg->dac, LL_DAC_CHANNEL_1);
	LL_TIM_EnableCounter(cfg->timer);
	irq_unlock(key);
	*actual_rate = data->rate;
	return 0;
}

static int initialize(const struct device *dev)
{
	const struct config *cfg = dev->config;
	struct data *data = dev->data;
	const struct device *clock = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
	if (!device_is_ready(clock) || !device_is_ready(cfg->dma) ||
	    !gpio_is_ready_dt(&cfg->enable)) {
		return -ENODEV;
	}
	int err = gpio_pin_configure_dt(&cfg->enable, GPIO_OUTPUT_INACTIVE);
	for (size_t i = 0; !err && i < 2; i++) {
		err = clock_control_on(clock, (clock_control_subsys_t)&cfg->clocks[i]);
		if (!err) {
			err = reset_line_toggle_dt(&cfg->resets[i]);
		}
	}
	if (!err) {
		err = clock_control_configure(clock, (clock_control_subsys_t)&cfg->clocks[2], NULL);
	}
	if (!err) {
		err = clock_control_get_rate(clock, (clock_control_subsys_t)&cfg->clocks[2], &data->clock);
	}
	if (!err) {
		err = pinctrl_apply_state(cfg->pins, PINCTRL_STATE_DEFAULT);
	}
	if (!err) {
		cfg->irq_setup();
	}
	return err;
}

static const struct sound_output_api api = {.start = start, .stop = stop};
PINCTRL_DT_INST_DEFINE(0);
static void irq_setup(void)
{
	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), underrun, DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));
}
static const struct config config = {
	.dac = (void *)DT_INST_REG_ADDR_BY_NAME(0, dac),
	.timer = (void *)DT_INST_REG_ADDR_BY_NAME(0, timer),
	.clocks = STM32_DT_INST_CLOCKS(0),
	.resets = {RESET_DT_SPEC_INST_GET_BY_IDX(0, 0), RESET_DT_SPEC_INST_GET_BY_IDX(0, 1)},
	.pins = PINCTRL_DT_INST_DEV_CONFIG_GET(0),
	.enable = GPIO_DT_SPEC_INST_GET(0, enable_gpios),
	.dma = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(0, tx)),
	.channel = DT_INST_DMAS_CELL_BY_NAME(0, tx, channel),
	.slot = DT_INST_DMAS_CELL_BY_NAME(0, tx, slot),
	.irq_setup = irq_setup,
};
static struct data data;
DEVICE_DT_INST_DEFINE(0, initialize, NULL, &data, &config, POST_KERNEL,
	CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &api);
