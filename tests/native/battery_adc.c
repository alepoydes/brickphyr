// SPDX-License-Identifier: Apache-2.0
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/init.h>

/* Synthetic quarter-scale current and three-quarter-scale voltage, before
 * the production service's first acquisition; shared by both ARM fixtures. */
static int initialize(void)
{
	const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc1));
	int err = adc_emul_const_raw_value_set(adc, 10, 1024);
	if (!err) {
		err = adc_emul_const_raw_value_set(adc, 11, 3072);
	}
	/* A 10 kohm thermistor at 25 C in the reference divider gives 2625. */
	return err ? err : adc_emul_const_raw_value_set(adc, 8, 2625);
}
SYS_INIT(initialize, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
