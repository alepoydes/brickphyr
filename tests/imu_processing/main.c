// SPDX-License-Identifier: Apache-2.0
#include <zephyr/ztest.h>

int imu_processing_checks(void);

ZTEST(imu_processing, test_calculation)
{
	zassert_equal(imu_processing_checks(), 0);
}

ZTEST_SUITE(imu_processing, NULL, NULL, NULL, NULL, NULL);
