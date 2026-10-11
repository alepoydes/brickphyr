// SPDX-License-Identifier: Apache-2.0

#include "hub_imu.h"
#include "hub_imu_internal.h"
#include <lsm6ds3tr_c_reg.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#define IMU DT_NODELABEL(hub_imu)
#define PERIOD_US DIV_ROUND_UP(1000000, 833)
/* Readers report staleness after four nominal periods. This is a validity
 * window, not a measured real-time scheduling guarantee. */
#define STALE_US (4 * PERIOD_US)
static const struct i2c_dt_spec bus = I2C_DT_SPEC_GET(IMU);
static const struct gpio_dt_spec irq = GPIO_DT_SPEC_GET(IMU, irq_gpios);
static const int32_t signs[] = DT_PROP(IMU, axis_signs);
BUILD_ASSERT(ARRAY_SIZE(signs) == 3);
BUILD_ASSERT((int32_t)DT_PROP_BY_IDX(IMU, axis_signs, 0) == 1 || (int32_t)DT_PROP_BY_IDX(IMU, axis_signs, 0) == -1);
BUILD_ASSERT((int32_t)DT_PROP_BY_IDX(IMU, axis_signs, 1) == 1 || (int32_t)DT_PROP_BY_IDX(IMU, axis_signs, 1) == -1);
BUILD_ASSERT((int32_t)DT_PROP_BY_IDX(IMU, axis_signs, 2) == 1 || (int32_t)DT_PROP_BY_IDX(IMU, axis_signs, 2) == -1);

static K_SEM_DEFINE(ready, 0, 1);
static struct gpio_callback callback;
static struct k_spinlock lock;
static atomic_t stopping;
static struct bp_imu_sample sample = {.error = -EINPROGRESS};
static int64_t edge_us;
static uint32_t edges;
static int io_error;
static bool identified;
static int64_t started_us;

static int64_t now_us(void)
{
	return k_cyc_to_us_floor64(k_cycle_get_64());
}

/* The reused ST/Pybricks register API has void I/O callbacks. Complete each
 * synchronously on this single worker, latch the first error, and suppress
 * subsequent transfers in a failed read/modify/write operation. */
static void read_register(void *context, uint8_t reg, uint8_t *data, uint16_t size)
{
	stmdev_ctx_t *ctx = context;
	if (!io_error) {
		io_error = i2c_burst_read_dt(&bus, reg, data, size);
	}
	ctx->read_write_done = true;
}

static void write_register(void *context, uint8_t reg, uint8_t *data, uint16_t size)
{
	stmdev_ctx_t *ctx = context;
	if (!io_error) {
		io_error = i2c_burst_write_dt(&bus, reg, data, size);
	}
	ctx->read_write_done = true;
}

static stmdev_ctx_t registers = {
	.read_reg = read_register,
	.write_reg = write_register,
	.handle = &registers,
};

/* Public continuation API; immediate callbacks make each operation finish in
 * one call. No PBIO event loop or HAL interrupt implementation is linked. */
#define CONFIGURE(call) do { \
	pbio_os_state_t step = 0; \
	io_error = 0; \
	pbio_error_t result = (call); \
	if (io_error) { return io_error; } \
	if (result != PBIO_SUCCESS) { return -EIO; } \
} while (0)

static int configure(void)
{
	uint8_t id;
	int err = i2c_reg_read_byte_dt(&bus, LSM6DS3TR_C_WHO_AM_I, &id);
	if (err) {
		/* A previous reset may have left the slave in a transfer. Try one
		 * standard bus recovery, preserving the original error on failure. */
		if (i2c_recover_bus(bus.bus)) {
			return err;
		}
		err = i2c_reg_read_byte_dt(&bus, LSM6DS3TR_C_WHO_AM_I, &id);
	}
	if (err || id != LSM6DS3TR_C_ID) {
		return err ? err : -ENODEV;
	}
	identified = true;
	CONFIGURE(lsm6ds3tr_c_reset_set(&step, &registers, PROPERTY_ENABLE));
	int64_t deadline = now_us() + 20000;
	for (;;) {
		uint8_t reset;
		CONFIGURE(lsm6ds3tr_c_reset_get(&step, &registers, &reset));
		if (!reset) {
			break;
		}
		if (now_us() >= deadline) {
			return -ETIMEDOUT;
		}
		k_msleep(1);
	}
	CONFIGURE(lsm6ds3tr_c_block_data_update_set(&step, &registers, PROPERTY_ENABLE));
	CONFIGURE(lsm6ds3tr_c_filter_settling_mask_set(&step, &registers, PROPERTY_ENABLE));
	CONFIGURE(lsm6ds3tr_c_xl_full_scale_set(&step, &registers, LSM6DS3TR_C_8g));
	CONFIGURE(lsm6ds3tr_c_gy_full_scale_set(&step, &registers, LSM6DS3TR_C_2000dps));
	CONFIGURE(lsm6ds3tr_c_data_ready_mode_set(&step, &registers, LSM6DS3TR_C_DRDY_PULSED));
	CONFIGURE(lsm6ds3tr_c_xl_data_rate_set(&step, &registers, LSM6DS3TR_C_XL_ODR_833Hz));
	CONFIGURE(lsm6ds3tr_c_gy_data_rate_set(&step, &registers, LSM6DS3TR_C_GY_ODR_833Hz));
	/* AN5130 table 14: gyro power-down to high-performance needs up to
	 * 70 ms. DRDY_MASK suppresses the additional digital-filter samples. */
	k_msleep(70);
	lsm6ds3tr_c_int1_route_t route = {.int1_drdy_g = 1};
	CONFIGURE(lsm6ds3tr_c_pin_int1_route_set(&step, &registers, route));
	return 0;
}

static void data_ready(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	edge_us = now_us();
	edges++;
	k_spin_unlock(&lock, key);
	k_sem_give(&ready);
}

static void failed(int error)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	sample.error = error;
	sample.read_us = now_us();
	sample.failures += sample.failures != UINT32_MAX;
	struct bp_imu_sample value = sample;
	k_spin_unlock(&lock, key);
	hub_imu_process(&value, NULL);
}

static void worker(void *a, void *b, void *c)
{
	if (!i2c_is_ready_dt(&bus) || !gpio_is_ready_dt(&irq)) {
		failed(-ENODEV);
		return;
	}
	/* AN5130 4.1: sensor calibration coefficients load for about 15 ms
	 * after power-on; do not depend on other boot work consuming that time. */
	k_msleep(15);
	int err = gpio_pin_configure_dt(&irq, GPIO_INPUT);
	if (!err) {
		gpio_init_callback(&callback, data_ready, BIT(irq.pin));
		err = gpio_add_callback(irq.port, &callback);
	}
	if (!err) {
		err = configure();
	}
	if (!err) {
		err = gpio_pin_interrupt_configure_dt(&irq, GPIO_INT_EDGE_TO_ACTIVE);
	}
	if (err) {
		goto finish;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	started_us = now_us();
	sample.error = -EAGAIN;
	sample.read_us = started_us;
	struct bp_imu_sample starting = sample;
	k_spin_unlock(&lock, key);
	hub_imu_process(&starting, NULL);
	uint32_t consumed = 0;
	bool resync = false;
	while (!atomic_get(&stopping)) {
		/* Freshness is computed by readers; a silent chip needs no polling. */
		k_sem_take(&ready, K_FOREVER);
		if (atomic_get(&stopping)) {
			break;
		}
		key = k_spin_lock(&lock);
		int64_t acquired = edge_us;
		uint32_t sequence = edges;
		k_spin_unlock(&lock, key);
		if (sequence == consumed) {
			continue;
		}
		uint32_t missed = sequence - consumed - 1;
		consumed = sequence;
		uint8_t data[12];
		err = i2c_burst_read_dt(&bus, LSM6DS3TR_C_OUTX_L_G, data, sizeof(data));
		int64_t completed = now_us();
		if (err) {
			/* A partial word can remain latched by BDU. Drain one complete
			 * frame before accepting samples from subsequent ready edges. */
			resync = true;
		} else if (resync) {
			resync = false;
			err = -EAGAIN;
		}
		struct bp_imu_sample next = {.sample_us = acquired, .read_us = completed,
			.sequence = sequence};
		int16_t raw[6];
		if (!err) {
			for (size_t i = 0; i < 3; i++) {
				raw[i] = sys_get_le16(data + 2 * i);
				raw[i + 3] = sys_get_le16(data + 6 + 2 * i);
				/* Convert before applying the mounting sign: -INT16_MIN
				 * is valid in these wider physical units. 9.81 matches PBIO. */
				next.angular_velocity[i] = signs[i] *
					lsm6ds3tr_c_from_fs2000dps_to_mdps(raw[i]);
				next.acceleration[i] = signs[i] *
					lsm6ds3tr_c_from_fs8g_to_mg(raw[i + 3]) * 9810.0f;
			}
		}
		key = k_spin_lock(&lock);
		/* Without FIFO there is no sample tag in the registers. Do not pair
		 * an overlapping update with the previous edge's timestamp. */
		if (!err && sequence != edges) {
			err = -EOVERFLOW;
		}
		missed += err != 0;
		next.missed = sample.missed + MIN(missed, UINT32_MAX - sample.missed);
		next.failures = sample.failures;
		if (!err) {
			sample = next;
		} else {
			sample.missed = next.missed;
			sample.failures += sample.failures != UINT32_MAX;
			sample.error = err;
			sample.read_us = completed;
			next = sample;
		}
		k_spin_unlock(&lock, key);
		/* Computation follows acquisition without holding an IRQ lock. */
		hub_imu_process(&next, err ? NULL : raw);
	}
	err = 0;
finish:
	/* Stop production before releasing power; preserve the first failure. */
	int off = gpio_pin_interrupt_configure_dt(&irq, GPIO_INT_DISABLE);
	int gyro = identified ? i2c_reg_write_byte_dt(&bus, LSM6DS3TR_C_CTRL2_G, 0) : 0;
	int accel = identified ? i2c_reg_write_byte_dt(&bus, LSM6DS3TR_C_CTRL1_XL, 0) : 0;
	err = err ? err : off ? off : gyro ? gyro : accel;
	if (err) {
		failed(err);
	} else {
		key = k_spin_lock(&lock);
		sample.error = -ESHUTDOWN;
		struct bp_imu_sample stopped = sample;
		k_spin_unlock(&lock, key);
		hub_imu_process(&stopped, NULL);
	}
}

/* 12 data bytes + address phases take about 338 us at 400 kHz, below the
 * 1201 us sample period. One 1536-byte stack and a binary wakeup, no sample
 * queue. Its 1.2 ms acquisition deadline precedes the 5 ms motor frame and
 * host processing. Cooperative supervisor/system work remains above it.
 * The actual interrupt latency and CPU share still require a physical board. */
K_THREAD_DEFINE(imu_thread, 1536, worker, NULL, NULL, NULL, 5, 0, 0);

int hub_imu_get(struct bp_imu_sample *value)
{
	if (!value) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	*value = sample;
	int64_t started = started_us;
	k_spin_unlock(&lock, key);
	if ((!value->error || (value->error == -EAGAIN && !value->sample_us)) && started &&
	    now_us() - (value->sample_us ? value->sample_us : started) > STALE_US) {
		value->error = -ETIMEDOUT;
	}
	return 0;
}

int hub_imu_shutdown(void)
{
	atomic_set(&stopping, 1);
	k_sem_give(&ready);
	/* Only this worker owns I2C2. Transfers and reset polling are bounded. */
	int err = k_thread_join(imu_thread, K_FOREVER);
	return err ? err : sample.error == -ESHUTDOWN ? 0 : sample.error;
}
