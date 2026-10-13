// SPDX-License-Identifier: Apache-2.0
#include "hub.h"
#include "hub_light.h"
#include "hub_power.h"
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/pwm/pwm_fake.h>
#include <zephyr/fff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

DEFINE_FFF_GLOBALS;
static uint32_t pulses[3];
static K_SEM_DEFINE(power_released, 0, 1);
static K_SEM_DEFINE(restarted, 0, 1);
static struct k_thread hub_thread;
static K_THREAD_STACK_DEFINE(hub_stack, 4096);

static int record_pwm(const struct device *dev, uint32_t channel, uint32_t period,
		      uint32_t pulse, pwm_flags_t flags)
{
	zassert_true(channel < 3);
	zassert_equal(period, channel == 2 ? 10000 : 1000);
	zassert_true(pulse <= period);
	pulses[channel] = pulse;
	return 0;
}

void hub_power_release(void)
{
	for (size_t i = 0; i < 3; i++) zassert_equal(pulses[i], 0);
	k_sem_give(&power_released);
}

void __wrap_sys_reboot(int type)
{
	zassert_equal(type, SYS_REBOOT_COLD);
	k_sem_give(&restarted);
	k_thread_abort(k_current_get());
	CODE_UNREACHABLE;
}

static void run_hub(void *a, void *b, void *c) { hub_run(); }

ZTEST(technic, test_gpio_button_and_pwm_lifecycle)
{
	fake_pwm_set_cycles_fake.custom_fake = record_pwm;
	const uint16_t raw[] = {UINT16_MAX, 0, 32768};
	zassert_ok(hub_light_pwm(true, raw));
	zassert_equal(pulses[0], 1000);
	zassert_equal(pulses[1], 0);
	zassert_equal(pulses[2], 5000);
	unsigned int writes = fake_pwm_set_cycles_fake.call_count;
	zassert_equal(hub_light_pwm(false, raw), -ENODEV);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, writes);
	struct bp_light_state state;
	zassert_equal(hub_light_get(BP_LIGHT_BLUETOOTH, &state), -ENODEV);
	/* Application colors, ownership and release use the same service. */
	const uint8_t red[] = {255, 0, 0};
	zassert_ok(hub_light_write(BP_LIGHT_CENTER, red));
	(void)hub_light_update(0, HOST_BLE_OFF);
	zassert_ok(hub_light_get(BP_LIGHT_CENTER, &state));
	zassert_true(state.owned && state.visible && state.valid && !state.pending);
	zassert_equal(pulses[0], 1000);
	zassert_equal(pulses[2], 0);
	zassert_ok(hub_light_release());

	const struct device *gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	zassert_ok(gpio_emul_input_set(gpio, 0, 1));
	k_msleep(40);
	k_thread_create(&hub_thread, hub_stack, K_THREAD_STACK_SIZEOF(hub_stack),
		run_hub, NULL, NULL, NULL, 1, 0, K_NO_WAIT);
	zassert_ok(gpio_emul_input_set(gpio, 0, 0));
	zassert_ok(k_sem_take(&power_released, K_MSEC(2100)));
	zassert_false(hub_power_connection(HUB_BLE));
	zassert_equal(hub_light_pwm(true, raw), -ESHUTDOWN);
	/* A held shutdown button cannot restart the still-powered MCU. */
	zassert_equal(k_sem_take(&restarted, K_MSEC(100)), -EAGAIN);
	zassert_ok(gpio_emul_input_set(gpio, 0, 1));
	k_msleep(40);
	zassert_ok(gpio_emul_input_set(gpio, 0, 0));
	zassert_ok(k_sem_take(&restarted, K_MSEC(100)));
	zassert_ok(k_thread_join(&hub_thread, K_MSEC(100)));
}

ZTEST_SUITE(technic, NULL, NULL, NULL, NULL, NULL);
