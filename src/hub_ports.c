// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2025 The Pybricks Authors
/* Electrical tests ported from lib/pbio/src/port_dcm_pup.c at a7b1b04ae.
 * Zephyr GPIO and a single delayed work item replace the PBIO scheduler.
 * See docs/ports.md for provenance, settling and identification limits. */
#include "hub_ports.h"
#include "port_uart.h"
#include "ports/output.h"
#include "hub_battery.h"
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
#include "motor/control.h"
#endif
#include <lego/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define PORTS DT_NODELABEL(hub_ports)
#define SETTLE_MS 2
#define MATCH_COUNT 20
#define RETRY_MS 100

enum pin { P5, P6, TX, RX, BUFFER, PIN_COUNT };
enum step { START, P6_HIGH, P6_LOW, TOUCH, ID1_HIGH, ID1_FLOAT, ID1_DONE,
	P5_HIGH, P5_LOW, RX_HIGH, RX_LOW, FINISH, UART };
enum group { GND, VCC, PULL_DOWN, OPEN };

static const uint16_t type_ids[3][3] = {
	{LEGO_DEVICE_TYPE_ID_LPF2_POWER, LEGO_DEVICE_TYPE_ID_LPF2_TURN,
	 LEGO_DEVICE_TYPE_ID_LPF2_LIGHT2},
	{LEGO_DEVICE_TYPE_ID_LPF2_TRAIN, LEGO_DEVICE_TYPE_ID_LPF2_LMOTOR,
	 LEGO_DEVICE_TYPE_ID_LPF2_LIGHT1},
	{LEGO_DEVICE_TYPE_ID_LPF2_MMOTOR, LEGO_DEVICE_TYPE_ID_LPF2_XMOTOR,
	 LEGO_DEVICE_TYPE_ID_LPF2_LIGHT},
};

static const struct gpio_dt_spec supply = GPIO_DT_SPEC_GET(PORTS, vcc_gpios);
#define PORT_PINS(node) [DT_REG_ADDR(node)] = { \
	GPIO_DT_SPEC_GET(node, p5_gpios), GPIO_DT_SPEC_GET(node, p6_gpios), \
	GPIO_DT_SPEC_GET(node, tx_gpios), GPIO_DT_SPEC_GET(node, rx_gpios), \
	GPIO_DT_SPEC_GET(node, buffer_gpios)},
static const struct gpio_dt_spec pins[BP_PORT_COUNT][PIN_COUNT] = {
	DT_FOREACH_CHILD_STATUS_OKAY(PORTS, PORT_PINS)
};
#define CHECK_INDEX(node) BUILD_ASSERT(DT_REG_ADDR(node) < BP_PORT_COUNT);
DT_FOREACH_CHILD_STATUS_OKAY(PORTS, CHECK_INDEX)

static struct port {
	struct bp_port_state value;
	struct bp_sensor_sample sensor;
	struct bp_motor_state motor;
	uint32_t motor_duration, motor_generation;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	struct motor_control controller; /* touched only by the port worker */
	int64_t control_due;
#endif
	uint32_t measurement, command;
	enum bp_port_owner owner;
	uint8_t supply;
	int64_t next_ms;
	enum step step;
	enum group group;
	uint16_t candidate, previous;
	uint8_t matches, pin_value, pressed;
} ports[BP_PORT_COUNT];
static struct k_spinlock lock;
static atomic_t stopping;
static atomic_t activity;
static uint32_t active_ports; /* lock protects mask; parity token is atomic */
static void scan(struct k_work *item);
static K_WORK_DELAYABLE_DEFINE(work, scan);
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
/* Model sampling must not queue behind system ADC/storage work. Priority 6
 * follows IMU (5) and precedes HCI TX/RX (7/8), management (10), Shell (13),
 * and applications (14) on F4.
 * Cooperative threads and interrupts still count against the frame deadline. */
static struct k_work_q port_queue;
K_THREAD_STACK_DEFINE(port_stack, 2048);
#define PORT_QUEUE (&port_queue)
#define CONTROL_MS PBIO_CONFIG_CONTROL_LOOP_TIME_MS
BUILD_ASSERT(CONFIG_SYS_CLOCK_TICKS_PER_SEC % (1000 / CONTROL_MS) == 0);
#else
#define PORT_QUEUE (&k_sys_work_q)
#endif

static void motor_activity(uint32_t port, bool active)
{
	bool before = active_ports != 0;
	WRITE_BIT(active_ports, port, active);
	if (before != (active_ports != 0)) atomic_inc(&activity);
}

uint32_t hub_motor_activity(void) { return atomic_get(&activity); }

/* Caller holds lock. One port claim covers measurement selection and drive. */
static void drop_claim(struct port *p)
{
	p->owner = p->sensor.owner = BP_PORT_UNOWNED;
	p->measurement = BP_SENSOR_DEFAULT;
	p->value.selected_mode = UINT8_MAX;
	p->value.mode_error = -ENOTSUP;
	p->command++;
	p->sensor.error = -EAGAIN;
	p->sensor.fields = 0;
}

static void invalidate_motor(uint32_t port, int reason)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bp_motor_state *m = &ports[port].motor;
	m->request++;
	m->pending = false;
	m->releasing = true; /* No new claim until the worker finishes cleanup. */
	m->requested_mode = BP_MOTOR_COAST;
	m->requested_duty = 0;
	m->deadline_ms = 0;
	if (!m->error) m->error = reason;
	k_spin_unlock(&lock, key);
}

static void finish_reset(uint32_t port, int cleanup)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	p->motor.pending = false;
	p->motor.completed = p->motor.request;
	p->motor.cleanup_error = cleanup;
	p->motor.releasing = cleanup != 0;
	if (cleanup) {
		p->motor.valid = false;
		if (!p->motor.error) p->motor.error = cleanup;
		motor_activity(port, true);
	} else {
		drop_claim(p);
	}
	k_spin_unlock(&lock, key);
}

static int set_pin(uint32_t port, enum pin pin, gpio_flags_t flags)
{
	return gpio_pin_configure_dt(&pins[port][pin], flags);
}

static int coast(uint32_t port)
{
	int result = port_output_coast(port);
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	ports[port].control_due = 0;
#endif
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bp_motor_state *m = &ports[port].motor;
	m->valid = !result;
	m->cleanup_error = result;
	m->control.active = false;
	if (!result) {
		ports[port].supply = 0;
		m->mode = BP_MOTOR_COAST;
		m->duty = 0;
		m->deadline_ms = 0;
		m->applied_ms = k_uptime_get();
	}
	motor_activity(port, result != 0);
	k_spin_unlock(&lock, key);
	return result;
}

static int sensor_supply(uint32_t port, uint8_t value)
{
	if (ports[port].supply == value) {
		return 0;
	}
	int err = port_output_supply(port, value);
	if (!err) {
		ports[port].supply = value;
	}
	return err;
}

/* Try all releases even after one fails. Disable the buffer before releasing
 * TX/RX; a failed detection must not leave a test output driven indefinitely. */
static int release_pins(uint32_t port)
{
	int result = gpio_is_ready_dt(&pins[port][BUFFER]) ?
		set_pin(port, BUFFER, GPIO_OUTPUT_INACTIVE) : -ENODEV;
	for (enum pin pin = P5; pin < BUFFER; pin++) {
		int err = gpio_is_ready_dt(&pins[port][pin]) ?
			set_pin(port, pin, GPIO_INPUT) : -ENODEV;
		if (!result) {
			result = err;
		}
	}
	int err = coast(port);
	if (!result) {
		result = err;
	}
	return result;
}

static void publish(uint32_t port, int error, int cleanup, const struct bp_port_state *uart,
	const struct bp_sensor_sample *sensor, uint32_t command)
{
	struct port *p = &ports[port];
	enum bp_port_phase phase = error ? BP_PORT_FAULT : uart ? uart->phase :
		p->matches < MATCH_COUNT ? BP_PORT_DETECTING :
		p->previous == LEGO_DEVICE_TYPE_ID_NONE ? BP_PORT_EMPTY :
		p->previous == LEGO_DEVICE_TYPE_ID_LPF2_UNKNOWN_UART ? BP_PORT_UART : BP_PORT_PASSIVE;
	uint16_t id = error ? 0 : uart ? uart->type_id :
		phase == BP_PORT_PASSIVE || phase == BP_PORT_UART ? p->previous : 0;
	int64_t now = k_uptime_get();
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bp_port_state v = uart ? *uart :
		(struct bp_port_state){.selected_mode = UINT8_MAX, .mode_error = -ENOTSUP};
	if (uart && p->command != command) {
		v.selected_mode = p->measurement & PORT_RAW_MODE ? p->measurement & 0xff : UINT8_MAX;
		v.mode_error = p->measurement & PORT_RAW_MODE ? -EAGAIN : -ENOTSUP;
	}
	v.generation = p->value.generation;
	v.changed_ms = p->value.changed_ms;
	if (p->value.phase != phase || p->value.type_id != id) {
		v.changed_ms = now;
		v.generation++;
		if (!p->owner && p->motor.valid && !p->motor.pending && !p->motor.releasing) {
			p->motor.error = 0;
		}
	}
	v.phase = phase;
	v.type_id = id;
	v.pressed = id == LEGO_DEVICE_TYPE_ID_LPF2_TOUCH ? p->pressed : 0;
	v.sample_ms = now;
	v.error = error ? error : uart ? uart->error : phase == BP_PORT_DETECTING ? -EAGAIN : 0;
	v.cleanup_error = cleanup;
	v.failures = p->value.failures;
	v.sequence = p->value.sequence;
	if (error) {
		v.failures += v.failures != UINT32_MAX;
	} else {
		v.sequence += uart ? uart->data_sequence - p->value.data_sequence : 1;
	}
	p->value = v;
	if (sensor && p->command == command) {
		p->sensor = *sensor;
	} else if (!sensor) {
		p->sensor = (struct bp_sensor_sample){.error = v.error ? v.error :
			phase == BP_PORT_EMPTY ? -ENODEV : -ENOTSUP};
	}
	p->sensor.type_id = id;
	p->sensor.generation = v.generation;
	p->sensor.owner = p->owner;
	k_spin_unlock(&lock, key);
}

#define TRY_IO(operation) do { int err_ = (operation); if (err_ < 0) { return err_; } } while (0)

/* Each return schedules at least 2 ms of settling. Reads never sleep or busy
 * wait, so six ports do not create six worker stacks or serialize their waits. */
static int advance(uint32_t port)
{
	struct port *p = &ports[port];
	int value;
	switch (p->step) {
	case UART: {
		struct bp_port_state uart = {0};
		struct bp_sensor_sample sensor;
		k_spinlock_key_t key = k_spin_lock(&lock);
		uint32_t measurement = p->measurement, command = p->command;
		k_spin_unlock(&lock, key);
		TRY_IO(port_uart_poll(port, &uart, &sensor, measurement));
		TRY_IO(sensor_supply(port, uart.supply));
		publish(port, 0, 0, &uart, &sensor, command);
		return 0;
	}
	case START:
		TRY_IO(release_pins(port));
		finish_reset(port, 0);
		p->matches = 0;
		p->previous = LEGO_DEVICE_TYPE_ID_NONE;
		break;
	case P6_HIGH:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P6]));
		p->pin_value = value;
		TRY_IO(set_pin(port, TX, GPIO_OUTPUT_LOW));
		p->step = P6_LOW;
		return 0;
	case P6_LOW:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P6]));
		if (p->pin_value == 1 && value == 0) {
			p->candidate = LEGO_DEVICE_TYPE_ID_LPF2_TOUCH;
			TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_INACTIVE));
			TRY_IO(set_pin(port, TX, GPIO_INPUT));
			p->step = TOUCH;
		} else if (p->pin_value == 0 && value == 1) {
			p->candidate = LEGO_DEVICE_TYPE_ID_LPF2_TPOINT;
			p->step = FINISH;
		} else {
			TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
			p->pin_value = value;
			TRY_IO(set_pin(port, TX, GPIO_OUTPUT_HIGH));
			p->step = ID1_HIGH;
		}
		return 0;
	case TOUCH:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
		p->pressed = !value;
		p->step = FINISH;
		return 0;
	case ID1_HIGH:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
		if (p->pin_value == value) {
			p->group = value ? VCC : GND;
			p->step = ID1_DONE;
		} else {
			TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_INACTIVE));
			TRY_IO(set_pin(port, TX, GPIO_INPUT));
			p->step = ID1_FLOAT;
		}
		return 0;
	case ID1_FLOAT:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
		p->group = value ? OPEN : PULL_DOWN;
		p->step = ID1_DONE;
		return 0;
	case ID1_DONE:
		TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_INACTIVE));
		TRY_IO(set_pin(port, TX, GPIO_INPUT));
		TRY_IO(set_pin(port, P6, GPIO_OUTPUT_HIGH));
		p->step = P5_HIGH;
		return 0;
	case P5_HIGH:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
		p->pin_value = value;
		TRY_IO(set_pin(port, P6, GPIO_OUTPUT_LOW));
		p->step = P5_LOW;
		return 0;
	case P5_LOW:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
		if (p->pin_value == 1 && value == 0) {
			if (p->group == OPEN) {
				p->candidate = LEGO_DEVICE_TYPE_ID_LPF2_3_PART;
			}
			p->step = FINISH;
		} else if (p->pin_value == 0 && value == 1) {
			p->candidate = LEGO_DEVICE_TYPE_ID_LPF2_EXPLOD;
			p->step = FINISH;
		} else {
			TRY_IO(set_pin(port, TX, GPIO_OUTPUT_HIGH));
			TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_ACTIVE));
			TRY_IO(set_pin(port, P6, GPIO_OUTPUT_HIGH));
			p->step = RX_HIGH;
		}
		return 0;
	case RX_HIGH:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][RX]));
		if (value) {
			TRY_IO(set_pin(port, P6, GPIO_OUTPUT_LOW));
			/* Preserve Pybricks' extra discharge of RX capacitance. */
			TRY_IO(set_pin(port, RX, GPIO_OUTPUT_LOW));
			p->step = RX_LOW;
		} else {
			p->candidate = p->group < OPEN ? type_ids[p->group][GND] :
				LEGO_DEVICE_TYPE_ID_LPF2_UNKNOWN_UART;
			p->step = FINISH;
		}
		return 0;
	case RX_LOW:
		TRY_IO(set_pin(port, RX, GPIO_INPUT));
		TRY_IO(value = gpio_pin_get_dt(&pins[port][RX]));
		if (p->group < OPEN) {
			p->candidate = type_ids[p->group][value ? VCC : PULL_DOWN];
		}
		p->step = FINISH;
		return 0;
	case FINISH:
		if (p->candidate == p->previous) {
			p->matches += p->matches < MATCH_COUNT;
		} else {
			invalidate_motor(port, -ENODEV);
			int cleanup = coast(port);
			finish_reset(port, cleanup);
			TRY_IO(cleanup);
			p->matches = 0;
			p->previous = p->candidate;
		}
		publish(port, 0, 0, NULL, NULL, 0);
		if (p->matches == MATCH_COUNT && p->previous == LEGO_DEVICE_TYPE_ID_LPF2_UNKNOWN_UART &&
		    port_uart_available(port)) {
			TRY_IO(release_pins(port));
			TRY_IO(port_uart_start(port));
			TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_ACTIVE));
			p->step = UART;
			return 0;
		}
		break;
	}
	TRY_IO(set_pin(port, P6, GPIO_INPUT));
	TRY_IO(set_pin(port, TX, GPIO_OUTPUT_HIGH));
	TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_ACTIVE));
	p->candidate = LEGO_DEVICE_TYPE_ID_NONE;
	p->group = OPEN;
	p->step = P6_HIGH;
	return 0;
}
#undef TRY_IO

/* Caller holds lock. Recognition alone never authorizes an unknown motor. */
static int motor_device_error(const struct port *p)
{
	if (p->value.error) return p->value.error;
	if (k_uptime_get() - p->value.sample_ms > 10 * (SETTLE_MS + k_ticks_to_ms_ceil64(2))) return -ETIMEDOUT;
	if (p->value.phase == BP_PORT_EMPTY) return -ENODEV;
	if (p->value.phase == BP_PORT_PASSIVE &&
	    (p->value.type_id == LEGO_DEVICE_TYPE_ID_LPF2_MMOTOR ||
	     p->value.type_id == LEGO_DEVICE_TYPE_ID_LPF2_TRAIN)) return 0;
	if (p->value.phase == BP_PORT_NEGOTIATING || p->value.phase == BP_PORT_UART) return -EAGAIN;
	if (p->value.phase != BP_PORT_READY || !(p->sensor.available & BIT(BP_SENSOR_ANGLE))) return -ENOTSUP;
	return p->sensor.error ? p->sensor.error :
		(p->sensor.fields & BP_SENSOR_HAS_ANGLE) ? 0 : -EAGAIN;
}

/* The one pending command is firmware-owned. A stop may replace it; another
 * drive cannot. No caller waits with a service lock or owns a driver buffer. */
static void set_motor_request(struct port *p, uint8_t mode, int32_t duty, uint32_t duration)
{
	p->motor.request++;
	p->motor.requested_ms = k_uptime_get();
	p->motor.requested_mode = mode;
	p->motor.requested_duty = duty;
	p->motor.pending = true;
	p->motor_duration = duration;
	p->motor_generation = p->value.generation;
	if (mode < BP_MOTOR_SPEED) p->motor.command = (struct bp_motor_command){0};
}

static int request_motor(struct port *p, uint8_t mode, int32_t duty, uint32_t duration)
{
	int err = k_work_reschedule_for_queue(PORT_QUEUE, &work, K_NO_WAIT);
	if (err < 0) return err;
	set_motor_request(p, mode, duty, duration);
	return 0;
}

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
static int control_input_error(const struct port *p)
{
	if (!(p->sensor.fields & BP_SENSOR_HAS_ANGLE)) return -ENOTSUP;
	/* Without a new angle over the entire numerical derivative window, the
	 * reported zero speed would be indistinguishable from missing feedback. */
	return k_uptime_get() - p->sensor.sample_ms >=
		PBIO_CONFIG_DIFFERENTIATOR_WINDOW_SIZE * CONTROL_MS ? -ETIMEDOUT : 0;
}

static int control_frame(struct port *p, uint16_t type, int64_t angle,
	const struct bp_motor_command *command, struct motor_reference *ref,
	int32_t *duty, int32_t *voltage, uint32_t ticks)
{
	struct bp_battery_sample battery;
	int err = hub_battery_get(&battery);
	if (!err) err = battery.error;
	if (err) return err;
	if (!battery.voltage_mv || battery.voltage_mv > 12000) return -ERANGE;
	if (!p->control_due) {
		err = motor_control_init(&p->controller, type, angle);
		if (err) return err;
	}
	if (command) {
		/* Enum values are an explicit adapter mapping, not SDK PBIO types. */
		BUILD_ASSERT((int)BP_MOTOR_FINISH_COAST == (int)PBIO_CONTROL_ON_COMPLETION_COAST);
		BUILD_ASSERT((int)BP_MOTOR_FINISH_BRAKE == (int)PBIO_CONTROL_ON_COMPLETION_BRAKE);
		BUILD_ASSERT((int)BP_MOTOR_FINISH_HOLD == (int)PBIO_CONTROL_ON_COMPLETION_HOLD);
		BUILD_ASSERT((int)BP_MOTOR_FINISH_CONTINUE == (int)PBIO_CONTROL_ON_COMPLETION_CONTINUE);
		err = command->mode == BP_MOTOR_SPEED ?
			motor_control_run(&p->controller, ticks, angle, command->speed_mdeg_s,
				command->duration_ms, command->finish) :
			motor_control_target(&p->controller, ticks, angle, command->target_mdeg,
				command->speed_mdeg_s, command->finish);
		if (err) return err;
	}
	err = motor_control_step(&p->controller, ticks, angle, ref);
	if (err) return err;
	*duty = CLAMP((int64_t)ref->voltage_mv * 1000 / battery.voltage_mv, -1000, 1000);
	*voltage = (int64_t)*duty * battery.voltage_mv / 1000;
	return 0;
}
#endif

static int64_t apply_motor(uint32_t port)
{
	struct port *p = &ports[port];
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bp_motor_state *m = &p->motor;
	int device_error = motor_device_error(p);
	bool running = m->mode != BP_MOTOR_COAST && m->valid;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	if (!device_error && (m->control.active || (m->pending && m->requested_mode >= BP_MOTOR_SPEED))) {
		device_error = control_input_error(p);
	}
#endif
	if (!m->pending && running && (device_error ||
	    (m->deadline_ms && k_uptime_get() >= m->deadline_ms))) {
		set_motor_request(p, BP_MOTOR_COAST, 0, 0);
		if (!m->error) m->error = device_error;
	}
	bool command_pending = m->pending;
	uint8_t mode = command_pending ? m->requested_mode : m->mode;
	bool controlled = mode >= BP_MOTOR_SPEED;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	int64_t next_frame = 0;
	if (controlled && (command_pending || m->control.active)) {
		next_frame = p->control_due;
		if (!device_error && next_frame > k_uptime_get()) {
			k_spin_unlock(&lock, key);
			return next_frame;
		}
	}
#endif
	if (!command_pending && !m->control.active) {
		int64_t deadline = m->deadline_ms;
		k_spin_unlock(&lock, key);
		return deadline;
	}
	uint32_t request = m->request, duration = p->motor_duration;
	int32_t duty = m->requested_duty;
	int err = mode != BP_MOTOR_COAST ? device_error ? device_error :
		p->motor_generation != p->value.generation ? -ENODEV : 0 : 0;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	struct bp_motor_command command = m->command;
	uint16_t type = p->value.type_id;
	int64_t angle = p->sensor.angle_mdeg;
	if (controlled && next_frame && k_uptime_get() >= next_frame + CONTROL_MS) {
		if (!err) err = -ETIMEDOUT;
		m->control.overruns += m->control.overruns != UINT32_MAX;
	}
#endif
	/* Mark activity before touching hardware, retaining it until a confirmed
	 * passive output. Even a start+stop between IMU samples changes the token. */
	if (!err && (controlled || (mode == BP_MOTOR_DUTY && duty))) motor_activity(port, true);
	m->valid = false;
	k_spin_unlock(&lock, key);
	bool passive = mode == BP_MOTOR_COAST;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	struct motor_reference ref = {0};
	int32_t voltage = 0;
	uint32_t ticks = k_ticks_to_us_floor64(k_uptime_ticks()) / 100;
	if (!err && controlled) {
		next_frame = (next_frame ? next_frame : k_uptime_get()) + CONTROL_MS;
		err = control_frame(p, type, angle, command_pending ? &command : NULL,
			&ref, &duty, &voltage, ticks);
		passive = ref.coast;
	}
#endif
	if (!err) err = passive ? port_output_coast(port) : port_output_drive(port, duty);
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	bool overrun = !err && controlled && k_uptime_get() >= next_frame;
	if (overrun) err = -ETIMEDOUT;
	if (!err && controlled) err = motor_control_predict(&p->controller, ticks, angle, passive, voltage);
	p->control_due = !err && controlled && ref.active ? next_frame : 0;
#endif
	int cleanup = !err ? 0 : mode == BP_MOTOR_COAST ? err : port_output_coast(port);
	key = k_spin_lock(&lock);
	m->completed = request;
	m->pending = request != m->request;
	m->valid = !cleanup;
	m->cleanup_error = cleanup;
	m->deadline_ms = 0;
	m->control.active = false;
	if (command_pending) m->control.done = m->control.stalled = false;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	if (overrun) m->control.overruns += m->control.overruns != UINT32_MAX;
	if (!err && controlled) {
		m->control.sample_ms = k_uptime_get();
		m->control.reference_mdeg = ref.angle_mdeg;
		m->control.reference_speed = ref.speed_mdeg_s;
		m->control.estimated_speed = ref.estimated_speed;
		m->control.requested_mv = ref.voltage_mv;
		m->control.applied_mv = voltage;
		m->control.active = ref.active;
		m->control.done = ref.done;
		m->control.stalled = ref.stalled;
		m->control.frames++;
	}
#endif
	if (err) {
		if (!m->error) m->error = err;
		m->failures += m->failures != UINT32_MAX;
	}
	if (m->valid) {
		m->mode = err || passive ? BP_MOTOR_COAST : controlled && !m->control.active ? BP_MOTOR_DUTY : mode;
		m->duty = err || passive ? 0 : duty;
		m->applied_ms = k_uptime_get();
		m->deadline_ms = !err && mode == BP_MOTOR_DUTY && duration ? m->applied_ms + duration : 0;
		p->supply = 0;
	}
	motor_activity(port, !m->valid || m->control.active || (m->mode != BP_MOTOR_COAST && m->duty));
	if (!m->pending && m->releasing && !err && mode == BP_MOTOR_COAST) {
		m->releasing = false;
		drop_claim(p);
	}
	int64_t deadline = m->deadline_ms;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	if (p->control_due) deadline = p->control_due;
#endif
	k_spin_unlock(&lock, key);
	return deadline;
}

static void scan(struct k_work *item)
{
	int64_t next = INT64_MAX;
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!pins[i][P5].port) {
			continue;
		}
		if (k_uptime_get() >= ports[i].next_ms) {
			int err = advance(i);
			if (err) {
				invalidate_motor(i, err);
				port_uart_stop(i);
				int cleanup = release_pins(i);
				finish_reset(i, cleanup);
				publish(i, err, cleanup, NULL, NULL, 0);
				ports[i].step = START;
			}
			ports[i].next_ms = k_uptime_get() + (err ? RETRY_MS : SETTLE_MS);
		}
		next = MIN(next, ports[i].next_ms);
		int64_t deadline = apply_motor(i);
		if (deadline) next = MIN(next, deadline);
	}
	if (!atomic_get(&stopping) && next != INT64_MAX) {
		k_work_schedule_for_queue(PORT_QUEUE, &work,
			IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL) ? K_TIMEOUT_ABS_MS(next) :
			K_MSEC(MAX(1, next - k_uptime_get())));
	}
}

static int initialize(void)
{
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	k_work_queue_start(PORT_QUEUE, port_stack, K_THREAD_STACK_SIZEOF(port_stack), 6, NULL);
#endif
	int err = gpio_is_ready_dt(&supply) ?
		gpio_pin_configure_dt(&supply, GPIO_OUTPUT_INACTIVE) : -ENODEV;
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		ports[i].value.error = -EAGAIN;
		ports[i].sensor.error = -EAGAIN;
		if (!pins[i][P5].port) {
			continue;
		}
		bool ready = true;
		for (enum pin pin = P5; pin < PIN_COUNT; pin++) {
			ready &= gpio_is_ready_dt(&pins[i][pin]);
		}
		int reset = ready ? release_pins(i) : -ENODEV;
		if (!err) {
			err = reset;
		}
	}
	if (!err) {
		err = gpio_pin_set_dt(&supply, 1);
	}
	if (err) {
		int cleanup = gpio_is_ready_dt(&supply) ? gpio_pin_set_dt(&supply, 0) : 0;
		for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
			publish(i, err, cleanup, NULL, NULL, 0);
		}
		return err;
	}
	k_work_schedule_for_queue(PORT_QUEUE, &work, K_MSEC(SETTLE_MS));
	return 0;
}
SYS_INIT(initialize, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

int hub_port_get(uint32_t port, struct bp_port_state *value)
{
	if (port >= BP_PORT_COUNT || !value) {
		return -EINVAL;
	}
	if (!pins[port][P5].port) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	*value = ports[port].value;
	k_spin_unlock(&lock, key);
	/* Longest probe: ten settling intervals. Include tick rounding at
	 * each wait; longer service starvation must not look like fresh identity. */
	int64_t max_age = 10 * (SETTLE_MS + k_ticks_to_ms_ceil64(2));
	if (!value->error && k_uptime_get() - value->sample_ms > max_age) {
		value->error = -ETIMEDOUT;
	}
	return 0;
}

int hub_port_get_mode(uint32_t port, uint32_t generation, uint32_t mode,
	struct bp_port_mode *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (port >= BP_PORT_COUNT || !value) return -EINVAL;
	if (!pins[port][P5].port) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	const struct bp_port_state *v = &ports[port].value;
	int err = atomic_get(&stopping) ? -ESHUTDOWN : v->generation != generation ? -ESTALE :
		v->phase == BP_PORT_EMPTY ? -ENODEV : v->phase == BP_PORT_PASSIVE ? -ENOTSUP :
		v->phase != BP_PORT_READY ? v->error ? v->error : -EAGAIN :
		k_uptime_get() - v->sample_ms > 10 * (SETTLE_MS + k_ticks_to_ms_ceil64(2)) ? -ETIMEDOUT : 0;
	if (!err) err = port_uart_get_mode(port, mode, value);
	k_spin_unlock(&lock, key);
	return err;
}

int hub_port_select(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t mode)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (port >= BP_PORT_COUNT || (owner != BP_PORT_APPLICATION && owner != BP_PORT_TERMINAL)) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = atomic_get(&stopping) ? -ESHUTDOWN :
		p->value.generation != generation ? -ESTALE :
		p->value.phase != BP_PORT_READY ? -ENODEV :
		p->value.error && p->value.error != -EAGAIN ? p->value.error :
		k_uptime_get() - p->value.sample_ms > 10 * (SETTLE_MS + k_ticks_to_ms_ceil64(2)) ? -ETIMEDOUT :
		mode >= p->value.num_modes ? -EINVAL :
		(p->owner && p->owner != owner) || p->motor.pending || p->motor.releasing ||
		!p->motor.valid || p->motor.control.active || p->motor.mode != BP_MOTOR_COAST ? -EBUSY : 0;
	if (!err) {
		p->owner = p->sensor.owner = owner;
		uint32_t request = PORT_RAW_MODE | mode;
		if (p->measurement != request) {
			p->measurement = request;
			p->command++;
			p->value.selected_mode = mode;
			p->value.mode_error = -EAGAIN;
			p->sensor.error = -ENOTSUP;
			p->sensor.fields = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_sensor_get(uint32_t port, struct bp_sensor_sample *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) {
		return -ENOTSUP;
	}
	if (port >= BP_PORT_COUNT || !value) {
		return -EINVAL;
	}
	if (!pins[port][P5].port) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	*value = ports[port].sensor;
	int64_t updated = ports[port].value.sample_ms;
	k_spin_unlock(&lock, key);
	if (!value->error && k_uptime_get() - updated > 10 * (SETTLE_MS + k_ticks_to_ms_ceil64(2))) {
		value->error = -ETIMEDOUT;
		value->fields = 0;
	}
	return 0;
}

int hub_sensor_configure(enum bp_port_owner owner, uint32_t port, uint32_t measurement)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) {
		return -ENOTSUP;
	}
	if (port >= BP_PORT_COUNT || measurement >= BP_SENSOR_MEASUREMENT_COUNT ||
	    (owner != BP_PORT_APPLICATION && owner != BP_PORT_TERMINAL)) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = atomic_get(&stopping) ? -ESHUTDOWN :
		p->motor.releasing ? -EBUSY :
		p->value.phase != BP_PORT_READY ? -EAGAIN :
		!(p->sensor.available & BIT(measurement)) ? -ENOTSUP :
		p->owner && p->owner != owner ? -EBUSY : 0;
	if (!err) {
		p->owner = owner;
		p->sensor.owner = owner;
		if (p->measurement != measurement) {
			p->measurement = measurement;
			p->command++;
			p->value.selected_mode = UINT8_MAX;
			p->value.mode_error = -ENOTSUP;
			p->sensor.error = -EAGAIN;
			p->sensor.fields = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_port_release(enum bp_port_owner owner, uint32_t port)
{
	if (port >= BP_PORT_COUNT || (owner != BP_PORT_APPLICATION && owner != BP_PORT_TERMINAL)) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = p->owner && p->owner != owner ? -EBUSY : 0;
	if (!err && p->owner) {
		if (p->motor.pending || p->motor.mode != BP_MOTOR_COAST || !p->motor.valid || p->motor.releasing) {
			p->motor.releasing = true;
			err = atomic_get(&stopping) ? -ESHUTDOWN : request_motor(p, BP_MOTOR_COAST, 0, 0);
			if (err) {
				p->motor.cleanup_error = err;
				if (!p->motor.error) p->motor.error = err;
			}
		} else {
			drop_claim(p);
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

void hub_ports_session_closed(enum bp_port_owner owner)
{
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		hub_port_release(owner, i);
	}
}

int hub_ports_release(enum bp_port_owner owner)
{
	if (owner != BP_PORT_APPLICATION && owner != BP_PORT_TERMINAL) return -EINVAL;
	int result = 0;
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool owned = ports[i].owner == owner;
		k_spin_unlock(&lock, key);
		int err = owned ? hub_port_release(owner, i) : 0;
		if (!result) result = err;
	}
	struct k_work_sync sync;
	for (;;) {
		bool pending = false;
		k_spinlock_key_t key = k_spin_lock(&lock);
		for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
			if (ports[i].owner != owner) continue;
			pending |= ports[i].motor.pending;
			if (!ports[i].motor.pending && !result) result = ports[i].motor.cleanup_error;
		}
		k_spin_unlock(&lock, key);
		if (!pending) return result;
		k_work_flush_delayable(&work, &sync);
	}
}

int hub_motor_get(uint32_t port, struct bp_motor_state *value)
{
	if (!value || port >= BP_PORT_COUNT) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	*value = ports[port].motor;
	value->owner = ports[port].owner;
	value->generation = ports[port].value.generation;
	if (!value->error) value->error = atomic_get(&stopping) ? -ESHUTDOWN : motor_device_error(&ports[port]);
	k_spin_unlock(&lock, key);
	return 0;
}

int hub_motor_drive(enum bp_port_owner owner, uint32_t port, int32_t duty, uint32_t duration_ms)
{
	if (port >= BP_PORT_COUNT || duty < -1000 || duty > 1000 || duration_ms > INT32_MAX ||
	    (owner != BP_PORT_APPLICATION && owner != BP_PORT_TERMINAL)) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	err = atomic_get(&stopping) ? -ESHUTDOWN : p->motor.releasing || p->motor.pending ||
		(p->owner && p->owner != owner) ? -EBUSY : motor_device_error(p);
	if (!err) err = request_motor(p, BP_MOTOR_DUTY, duty, duration_ms);
	if (!err) {
		p->owner = p->sensor.owner = owner;
		p->motor.error = p->motor.cleanup_error = 0;
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_motor_control(enum bp_port_owner owner, uint32_t port, const struct bp_motor_command *command)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (!command || port >= BP_PORT_COUNT ||
	    (owner != BP_PORT_APPLICATION && owner != BP_PORT_TERMINAL)) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	struct bp_battery_sample battery;
	err = hub_battery_get(&battery);
	if (!err) err = battery.error;
	if (!err && (!battery.voltage_mv || battery.voltage_mv > 12000)) err = -ERANGE;
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	err = atomic_get(&stopping) ? -ESHUTDOWN : p->motor.releasing || p->motor.pending ||
		(p->owner && p->owner != owner) ? -EBUSY : motor_device_error(p);
	if (!err) err = control_input_error(p);
	if (!err) err = motor_control_check(p->value.type_id, p->sensor.angle_mdeg, command);
	if (!err) err = request_motor(p, command->mode, 0, 0);
	if (!err) {
		p->motor.command = *command;
		p->owner = p->sensor.owner = owner;
		p->motor.error = p->motor.cleanup_error = 0;
	}
	k_spin_unlock(&lock, key);
	return err;
#endif
}

int hub_motor_coast(enum bp_port_owner owner, uint32_t port)
{
	if (port >= BP_PORT_COUNT || (owner != BP_PORT_APPLICATION && owner != BP_PORT_TERMINAL)) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	err = atomic_get(&stopping) ? -ESHUTDOWN :
		p->owner && p->owner != owner ? -EBUSY :
		p->owner && (p->motor.pending || p->motor.mode != BP_MOTOR_COAST ||
			!p->motor.valid || p->motor.releasing) ? 0 : motor_device_error(p);
	if (!err) err = request_motor(p, BP_MOTOR_COAST, 0, 0);
	if (!err) {
		p->owner = p->sensor.owner = owner;
		if (!p->motor.releasing) p->motor.error = p->motor.cleanup_error = 0;
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_ports_shutdown(void)
{
	atomic_set(&stopping, 1);
	struct k_work_sync sync;
	k_work_cancel_delayable_sync(&work, &sync);
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		port_uart_stop(i);
	}
	int supply_error = gpio_is_ready_dt(&supply) ? gpio_pin_set_dt(&supply, 0) : -ENODEV;
	int result = supply_error;
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!pins[i][P5].port) {
			continue;
		}
		invalidate_motor(i, -ESHUTDOWN);
		int err = release_pins(i);
		finish_reset(i, err);
		if (!result) {
			result = err;
		}
		k_spinlock_key_t key = k_spin_lock(&lock);
		struct bp_port_state *v = &ports[i].value;
		int cleanup = supply_error ? supply_error : err;
		enum bp_port_phase phase = cleanup ? BP_PORT_FAULT : BP_PORT_OFF;
		if (v->phase != phase || v->type_id) {
			v->generation++;
			v->changed_ms = k_uptime_get();
		}
		v->phase = phase;
		v->type_id = 0;
		v->pressed = 0;
		v->length = 0;
		v->supply = 0;
		ports[i].sensor.fields = 0;
		ports[i].sensor.available = 0;
		ports[i].sensor.type_id = 0;
		ports[i].sensor.generation = v->generation;
		if (!ports[i].sensor.error || ports[i].sensor.error == -EAGAIN) {
			ports[i].sensor.error = cleanup ? cleanup : -ESHUTDOWN;
		}
		if (!v->error || v->error == -EAGAIN) {
			v->error = cleanup ? cleanup : -ESHUTDOWN;
		}
		v->cleanup_error = cleanup;
		k_spin_unlock(&lock, key);
	}
	return result;
}
