// SPDX-License-Identifier: Apache-2.0
#include "hub.h"
#include "hub_light.h"
#include "hub_power.h"
#include "input_resistor_ladder.h"
#include <string.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/input/input.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

/* Keep the production hub loop, input driver and power policy. Only outputs
 * requiring the board and the terminal's transport are substituted. */
static K_SEM_DEFINE(power_released, 0, 1);
static K_SEM_DEFINE(restarted, 0, 1);
static unsigned int releases, lights_off;

void hub_power_release(void)
{
	zassert_equal(lights_off, 1);
	releases++;
	k_sem_give(&power_released);
}

void __wrap_sys_reboot(int type)
{
	zassert_equal(type, SYS_REBOOT_COLD);
	k_sem_give(&restarted);
	k_thread_abort(k_current_get());
	CODE_UNREACHABLE;
}

int hub_light_off(void) { lights_off++; return 0; }
int64_t hub_light_update(int64_t now) { zassert_equal(lights_off, 0); return now + 20; }
int hub_light_select(enum host_mode mode, bool menu, int64_t now) { return 0; }
int hub_light_manual(bool center, const uint8_t rgb[3]) { return 0; }
int hub_light_pwm(bool center, const uint16_t duty[3]) { return 0; }
enum host_mode host_interface_current(void) { return HOST_SHELL; }
int host_interface_select(uint16_t mode) { return 0; }
void host_interface_status(uint8_t state[8]) { memset(state, 0, 8); }

static void run_hub(void *a, void *b, void *c) { hub_run(); }
static struct k_thread hub_thread;
static K_THREAD_STACK_DEFINE(hub_stack, 4096);

static int bad_adc(const struct device *dev, unsigned int channel, void *data, uint32_t *raw)
{
	return -EIO;
}

static int bouncing_adc(const struct device *dev, unsigned int channel, void *data, uint32_t *raw)
{
	unsigned int *samples = data;
	*raw = (*samples)++ % 2 ? 3144 : 4073;
	return 0;
}

ZTEST(poweroff, test_shutdown_requires_real_release_and_new_press)
{
	const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc1));
	const struct device *center = DEVICE_DT_GET(DT_NODELABEL(center_keys));
	const struct device *gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	const struct shell *shell = shell_backend_dummy_get_ptr();

	zassert_ok(gpio_pin_configure(gpio, 0, GPIO_INPUT));
	zassert_ok(gpio_emul_input_set(gpio, 0, 1));
	zassert_ok(adc_emul_const_raw_value_set(adc, 1, 4073));
	zassert_ok(adc_emul_const_raw_value_set(adc, 14, 3144));
	k_msleep(80);
	zassert_equal(resistor_ladder_get_key(center, INPUT_KEY_ENTER), 1);
	k_thread_create(&hub_thread, hub_stack, K_THREAD_STACK_SIZEOF(hub_stack),
		run_hub, NULL, NULL, NULL, 1, 0, K_NO_WAIT);

	/* Explicit shutdown must work even with a management owner and held key. */
	zassert_true(hub_power_connection(true));
	zassert_ok(shell_execute_cmd(shell, "shutdown"));
	zassert_ok(k_sem_take(&power_released, K_MSEC(100)));
	zassert_false(hub_power_connection(true), "Shutdown admitted another owner");
	zassert_equal(k_sem_take(&restarted, K_SECONDS(3)), -EAGAIN,
		"The shutdown hold restarted the hub");

	/* The driver's synthetic release on ADC failure is not a wake gesture. */
	zassert_ok(adc_emul_raw_value_func_set(adc, 14, bad_adc, NULL));
	k_msleep(30);
	zassert_equal(resistor_ladder_get_key(center, INPUT_KEY_ENTER), -EIO);
	zassert_ok(adc_emul_const_raw_value_set(adc, 14, 3144));
	zassert_equal(k_sem_take(&restarted, K_MSEC(50)), -EAGAIN);
	unsigned int samples = 0;
	zassert_ok(adc_emul_raw_value_func_set(adc, 14, bouncing_adc, &samples));
	zassert_equal(k_sem_take(&restarted, K_MSEC(80)), -EAGAIN, "Bounce armed restart");
	zassert_ok(adc_emul_const_raw_value_set(adc, 14, 3144));
	zassert_equal(k_sem_take(&restarted, K_MSEC(30)), -EAGAIN);

	zassert_ok(adc_emul_const_raw_value_set(adc, 14, 4073));
	k_msleep(30);
	zassert_equal(resistor_ladder_get_key(center, INPUT_KEY_ENTER), 0);
	/* Other buttons cannot turn on a hub whose central key was released. */
	zassert_ok(adc_emul_const_raw_value_set(adc, 1, 2200));
	zassert_equal(k_sem_take(&restarted, K_MSEC(50)), -EAGAIN);
	zassert_ok(adc_emul_const_raw_value_set(adc, 14, 3144));
	zassert_ok(k_sem_take(&restarted, K_MSEC(100)));
	zassert_ok(k_thread_join(&hub_thread, K_MSEC(100)));
	zassert_equal(releases, 1);
}

ZTEST_SUITE(poweroff, NULL, NULL, NULL, NULL, NULL);
