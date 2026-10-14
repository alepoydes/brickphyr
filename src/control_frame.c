// SPDX-License-Identifier: Apache-2.0
#include "control_frame.h"
#include "hub_imu.h"
#include "hub_battery.h"
#include "hub_ports.h"
#include <string.h>
#include <zephyr/kernel.h>

int64_t control_time_us(void)
{
	if (IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER)) {
		return k_cyc_to_us_floor64(k_cycle_get_64());
	}
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

static int begin(uint32_t port_mask, struct bp_control_frame *frame)
{
	if (!frame || port_mask >> BP_PORT_COUNT) return -EINVAL;
	/* Caller owns storage; never keep a pointer or allocate a second frame.
	 * Each service's bounded copy has its own short lock. */
	int64_t started = control_time_us();
	memset(frame, 0, sizeof(*frame));
	frame->capture_start_us = started;
	frame->port_mask = port_mask;
	frame->capture_resolution_us = DIV_ROUND_UP(1000000,
		IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER) ?
		CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC : CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	return 0;
}

static void finish(struct bp_control_frame *frame)
{
	int err = hub_battery_get(&frame->battery);
	if (err) frame->battery.error = frame->battery.temperature_error = err;
	for (uint32_t port = 0; port < BP_PORT_COUNT; port++) {
		if (!(frame->port_mask & BIT(port))) continue;
		err = hub_control_port_get(port, &frame->ports[port]);
		if (err) frame->ports[port].error = frame->ports[port].output_error = err;
	}
	frame->capture_end_us = control_time_us();
}

int control_frame_get(uint32_t port_mask, struct bp_control_frame *frame)
{
	int err = begin(port_mask, frame);
	if (err) return err;
	err = hub_imu_get_state(&frame->imu);
	if (err) frame->imu.error = frame->imu.raw.error = err;
	finish(frame);
	return 0;
}

int control_frame_imu_wait(uint32_t port_mask, uint32_t sequence, bool previous,
	uint32_t timeout_ms, struct bp_control_frame *frame)
{
	int err = begin(port_mask, frame);
	if (err) return err;
	err = hub_imu_wait_state(sequence, previous, timeout_ms, &frame->imu);
	if (err) {
		frame->imu.error = err;
		frame->capture_end_us = control_time_us();
		return err;
	}
	/* Keep the triggering IMU bytes. A newer sample may be published while
	 * collecting the other sources; it belongs to a later control step. */
	finish(frame);
	return 0;
}
