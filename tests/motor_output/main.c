// SPDX-License-Identifier: Apache-2.0
#include "output.h"
#include "wiring.h"
#include <zephyr/drivers/pwm/pwm_fake.h>
#include <zephyr/dt-bindings/pwm/stm32_pwm.h>
#include <zephyr/ztest.h>

DEFINE_FFF_GLOBALS;

ZTEST(motor_output, test_drive_supply_and_coast)
{
	/* PWM's normal-polarity test controller records timer counts. The F4
	 * DT uses inverted polarity; this does not emulate STM32 pinmux/timers. */
	for (uint32_t port = 0; port < 4; port++) {
		zassert_ok(port_output_coast(port));
		zassert_equal(pup_test_sensor_supply(port), 0);
		zassert_ok(port_output_drive(port, 250));
		zassert_equal(fake_pwm_set_cycles_fake.arg1_val, port * 2);
		zassert_equal(fake_pwm_set_cycles_fake.arg2_val, 1000);
		zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 250);
		zassert_equal(pup_test_sensor_supply(port), 2, "Forward fixed leg");
		uint32_t writes = pup_test_circuits[port].writes;
		unsigned calls = fake_pwm_set_cycles_fake.call_count;
		zassert_ok(port_output_drive(port, 1));
		zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 1, "Lost the smallest duty step");
		zassert_equal(fake_pwm_set_cycles_fake.call_count, calls + 1);
		zassert_equal(pup_test_circuits[port].writes, writes, "Remuxed a steady direction");
		zassert_ok(port_output_drive(port, 1000));
		zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 1000);
		zassert_ok(port_output_drive(port, -750));
		zassert_equal(fake_pwm_set_cycles_fake.arg1_val, port * 2 + 1);
		zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 750);
		zassert_equal(pup_test_sensor_supply(port), 1, "Reverse fixed leg");
		zassert_ok(port_output_drive(port, 0));
		zassert_equal(pup_test_sensor_supply(port), 3, "Zero duty must brake");
		zassert_ok(port_output_coast(port));
		zassert_equal(pup_test_sensor_supply(port), 0);
		zassert_equal(fake_pwm_set_cycles_fake.arg2_val, 0, "Coast left PWM enabled");
		zassert_ok(port_output_supply(port, 1));
		zassert_equal(pup_test_sensor_supply(port), 1);
		zassert_ok(port_output_supply(port, 2));
		zassert_equal(pup_test_sensor_supply(port), 2);
		zassert_ok(port_output_supply(port, 0));
		zassert_equal(pup_test_sensor_supply(port), 0);
	}
	unsigned calls = fake_pwm_set_cycles_fake.call_count;
	uint32_t writes = pup_test_circuits[5].writes;
	zassert_equal(port_output_drive(5, 100), -ENOTSUP);
	zassert_equal(port_output_drive(6, 100), -EINVAL);
	zassert_equal(port_output_drive(0, INT32_MIN), -EINVAL);
	zassert_equal(port_output_drive(0, 1001), -EINVAL);
	zassert_equal(port_output_supply(0, 3), -EINVAL);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, calls);
	zassert_equal(pup_test_circuits[5].writes, writes);
}

ZTEST(motor_output, test_shared_compare_complementary_outputs)
{
	RESET_FAKE(fake_pwm_set_cycles);
	zassert_ok(port_output_drive(4, 250));
	zassert_equal(fake_pwm_set_cycles_fake.arg1_val, 8);
	zassert_equal(fake_pwm_set_cycles_fake.arg2_val, 1200);
	zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 300);
	zassert_equal(fake_pwm_set_cycles_fake.arg4_val, STM32_PWM_COMPLEMENTARY);
	zassert_equal(pup_test_sensor_supply(4), 2);
	unsigned calls = fake_pwm_set_cycles_fake.call_count;
	zassert_ok(port_output_drive(4, -750));
	/* Changing direction must disable both legs of the shared channel before
	 * configuring the selected output. Neither channel number alone nor one
	 * polarity value can identify these two physical outputs. */
	zassert_equal(fake_pwm_set_cycles_fake.call_count, calls + 3);
	zassert_equal(fake_pwm_set_cycles_fake.arg2_history[calls], 0);
	zassert_equal(fake_pwm_set_cycles_fake.arg4_history[calls], STM32_PWM_COMPLEMENTARY);
	zassert_equal(fake_pwm_set_cycles_fake.arg2_history[calls + 1], 0);
	zassert_equal(fake_pwm_set_cycles_fake.arg4_history[calls + 1], PWM_POLARITY_INVERTED);
	zassert_equal(fake_pwm_set_cycles_fake.arg1_val, 8);
	zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 900);
	zassert_equal(fake_pwm_set_cycles_fake.arg4_val, PWM_POLARITY_INVERTED);
	zassert_equal(pup_test_sensor_supply(4), 1);
	zassert_ok(port_output_coast(4));
	zassert_equal(pup_test_sensor_supply(4), 0);
}

ZTEST(motor_output, test_failed_output_cleanup)
{
	zassert_ok(port_output_drive(0, 500));
	fake_pwm_set_cycles_fake.return_val = -EIO;
	zassert_equal(port_output_drive(0, 600), -EIO);
	/* The caller retains the command error, then attempts every release even
	 * when one pin and both timer-disable operations fail. */
	pup_test_circuits[0].coast_error = -EACCES;
	unsigned calls = fake_pwm_set_cycles_fake.call_count;
	zassert_equal(port_output_coast(0), -EACCES);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, calls + 2);
	zassert_equal(pup_test_sensor_supply(0), 0, "Other leg was not released");
	fake_pwm_set_cycles_fake.return_val = 0;
	zassert_ok(port_output_coast(0));
	zassert_ok(port_output_drive(0, -1000));
	zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 1000);
	zassert_equal(pup_test_sensor_supply(0), 1);
	zassert_ok(port_output_coast(0));
}

ZTEST_SUITE(motor_output, NULL, NULL, NULL, NULL, NULL);
