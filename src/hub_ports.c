// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2025 The Pybricks Authors
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2023 LEGO System A/S
/* Electrical tests ported from lib/pbio/src/port_dcm_pup.c at a7b1b04ae.
 * DC direction/voltage semantics follow lib/pbio/src/dcmotor.c there.
 * Zephyr GPIO and a single delayed work item replace the PBIO scheduler.
 * See docs/ports.md for provenance, settling and identification limits. */
#define __ZEPHYR_SUPERVISOR__
#include "hub_ports.h"
#include "port_uart.h"
#include "uart_settings.h"
#include "ports/output.h"
#include "ports/sensor.h"
#include "hub_battery.h"
#include "hub_watchdog.h"
#include "motor/voltage.h"
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
#include "motor/control.h"
#include "motor/drivebase.h"
#include "hub_imu.h"
#include <math.h>
#endif
#include <lego/device.h>
#include <lego/lump.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hub_ports, LOG_LEVEL_WRN);

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
static const struct gpio_dt_spec pins[][PIN_COUNT] = {
	DT_FOREACH_CHILD_STATUS_OKAY(PORTS, PORT_PINS)
};
#define CHECK_INDEX(node) BUILD_ASSERT(DT_REG_ADDR(node) < BP_PORT_COUNT);
DT_FOREACH_CHILD_STATUS_OKAY(PORTS, CHECK_INDEX)

/* Public port numbers stay A..F; storage follows the board's highest index. */
static bool port_present(uint32_t port)
{
	return port < ARRAY_SIZE(pins) && pins[port][P5].port;
}

static bool owner_valid(enum bp_port_owner owner)
{
	return owner == BP_PORT_APPLICATION || owner == BP_PORT_TERMINAL || owner == BP_PORT_CONTROLLER;
}

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
struct motor_log {
	int32_t *data;
	struct bp_motor_log_status status;
	enum bp_port_owner owner;
	uint32_t generation, down_sample, skipped, start_ms;
};
#endif

struct port_uart_state {
	struct bp_sensor_sample sensor;
	int64_t reset_deadline;
	struct bp_sensor_palette palette;
	struct port_uart_write write;
	int pf_error;
	uint32_t pf_generation;
	enum { SERIAL_OFF, SERIAL_CONFIGURE, SERIAL_ACTIVE, SERIAL_CLOSE, SERIAL_FAILING, SERIAL_FAILED } serial;
	uint32_t serial_baud;
	uint32_t measurement, command;
	uint16_t reset_type;
	/* One bit per remote channel/output; no application memory is retained. */
	uint8_t pf_used, pf_coasting;
	bool reset_start;
	uint8_t serial_power;
};

static struct port {
	struct bp_port_state value;
#ifdef CONFIG_BRICKPHYR_PORT_UART
	struct port_uart_state uart;
#endif
	/* Internal generation tags the last completed output. hub_motor_get keeps
	 * its existing public meaning: the current port connection generation. */
	struct bp_motor_state motor;
	uint32_t motor_duration, motor_generation;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	struct drivebase *drivebase;
	struct motor_control *controller; /* stable storage, computed only by the worker */
	int64_t control_due;
	/* Publication copy: readers never lock out computation or inspect the
	 * observer while the worker mutates it. Copied once per 5 ms frame. */
	pbio_differentiator_t speed_history;
	struct bp_motor_observation observation;
	pbio_control_settings_t control_settings;
	pbio_observer_settings_t model_settings;
	struct bp_motor_model_state model_state;
	struct motor_log log[BP_MOTOR_LOG_COUNT];
	struct bp_servo_trajectory trajectory;
	uint32_t settings_revision, settings_applied, zero_generation;
	bool settings_ready;
	uint32_t geometry_revision, observation_revision;
	bool observer_reset, observer_rebase;
#endif
	enum bp_port_owner owner;
	uint8_t supply;
	int64_t next_ms;
	enum step step;
	enum group group;
	uint16_t candidate, previous;
	uint8_t matches, pin_value, pressed;
} ports[ARRAY_SIZE(pins)];
/* Only UART operations access the optional state. Common cleanup reads
 * inactive state through these helpers, so it needs no disabled storage. */
#ifdef CONFIG_BRICKPHYR_PORT_UART
static struct port_uart_state *uart_state(struct port *p) { return &p->uart; }
static uint8_t pf_outputs(const struct port *p) { return p->uart.pf_used; }
static int pf_failure(const struct port *p) { return p->uart.pf_error; }
static int serial_mode(const struct port *p) { return p->uart.serial; }
#else
static struct port_uart_state *uart_state(struct port *p) { ARG_UNUSED(p); return NULL; }
static uint8_t pf_outputs(const struct port *p) { ARG_UNUSED(p); return 0; }
static int pf_failure(const struct port *p) { ARG_UNUSED(p); return 0; }
static int serial_mode(const struct port *p) { ARG_UNUSED(p); return SERIAL_OFF; }
#endif

static struct k_spinlock lock;
static atomic_t stopping;
static atomic_t activity;
static uint32_t active_ports; /* lock protects mask; parity token is atomic */
#ifdef CONFIG_BRICKPHYR_MOTORS
/* One raw-output group shares the existing port worker and pending commands.
 * Fixed, small metadata avoids allocation in a controller's first step too. */
static struct {
	struct bp_motor_group_state value;
	enum bp_port_owner owner;
	bool cancelling;
} motor_group;

/* Caller holds lock. Completed/cancelled history does not reserve ports. */
static bool group_member(const struct port *p)
{
	return (motor_group.value.active || motor_group.value.pending) &&
		(motor_group.value.port_mask & BIT(p - ports));
}
#else
static bool group_member(const struct port *p) { ARG_UNUSED(p); return false; }
#endif
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
/* Membership/publication uses lock; only the existing port worker computes. */
struct drivebase {
	struct port *motor[2];
	uint32_t handle;
	enum bp_port_owner owner;
	struct bp_drivebase_setup setup;
	struct bp_drivebase_state value;
	struct bp_drivebase_command command;
	struct bp_drivebase_settings settings;
	struct bp_drivebase_axis_state axis[DRIVEBASE_AXES];
	struct bp_servo_settings axis_settings;
	uint32_t settings_axis, settings_group;
	struct motor_log log[DRIVEBASE_AXES];
	struct drivebase_control controller;
	struct drivebase_reference reference;
	uint32_t applied, imu_epoch;
	enum { DRIVE_CONFIG_NONE, DRIVE_CONFIG_BASIC, DRIVE_CONFIG_AXIS } configure;
	bool initializing, cancelled, detach, frame;
};
/* Slots bound the number of pairs, but unused controllers borrow no heap.
 * Only the worker retires a live pair; stopped logs retain its record until
 * owner cleanup or slot reuse. All pointer publication/reclamation holds lock. */
static struct drivebase *drivebases[ARRAY_SIZE(pins) / 2];
static uint32_t drivebase_handle;
static void cancel_drivebase(struct port *p, int error, bool detach);
#endif
static void scan(struct k_work *item);
static K_WORK_DELAYABLE_DEFINE(work, scan);
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
/* Model sampling must not queue behind system ADC/storage work. Priority 6
 * follows IMU (5) and precedes HCI TX/RX (7/8), management (10), Shell (13),
 * and applications (14) on F4.
 * Cooperative threads and interrupts still count against the frame deadline. */
static struct k_work_q port_queue;
/* ARM static call depth reaches about 2 KiB for a paired command, before
 * exception/FPU context. Leave room for shared IMU snapshot conversion. */
K_KERNEL_STACK_DEFINE(port_stack, 3072);
#define PORT_QUEUE (&port_queue)
#define CONTROL_MS PBIO_CONFIG_CONTROL_LOOP_TIME_MS
BUILD_ASSERT(CONFIG_SYS_CLOCK_TICKS_PER_SEC % (1000 / CONTROL_MS) == 0);
#else
#define PORT_QUEUE (&k_sys_work_q)
#endif

static void motor_activity(uint32_t port, bool active)
{
	active |= pf_outputs(&ports[port]) != 0;
	bool before = active_ports != 0;
	WRITE_BIT(active_ports, port, active);
	if (before != (active_ports != 0)) atomic_inc(&activity);
}

uint32_t hub_motor_activity(void) { return atomic_get(&activity); }

static bool write_pending(const struct port *p)
{
	return IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART) && p->value.write.request != p->value.write.completed;
}

static bool reset_pending(const struct port *p)
{
	return IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART) && p->value.reset.request != p->value.reset.completed;
}

/* Caller holds lock. Retain the exact result across later connections. */
static void finish_resync(struct port *p, int error, uint32_t generation)
{
	if (reset_pending(p)) {
		p->value.reset.completed = p->value.reset.request;
		p->value.reset.error = error;
		p->value.reset.generation = generation;
		uart_state(p)->reset_start = false;
	}
}

/* Caller holds lock. A failed or cancelled operation keeps its own result. */
static void finish_write(struct port *p, int error)
{
	if (write_pending(p)) {
		p->value.write.completed = p->value.write.request;
		p->value.write.error = error;
		if (uart_state(p)->pf_coasting) {
			if (error) {
				if (!pf_failure(p)) uart_state(p)->pf_error = error;
			} else {
				uart_state(p)->pf_used &= ~uart_state(p)->pf_coasting;
			}
			uart_state(p)->pf_coasting = 0;
		}
	}
}

/* All request state is copied while holding lock. The worker is its only
 * consumer; callers cannot overwrite it before completion. */
static uint32_t queue_write(struct port *p, uint8_t mode, const uint8_t *data, uint32_t size)
{
	uint32_t selection = PORT_RAW_MODE | mode;
	if (!serial_mode(p) && uart_state(p)->measurement != selection) {
		uart_state(p)->measurement = selection;
		uart_state(p)->command++;
		p->value.selected_mode = mode;
		p->value.mode_error = -EAGAIN;
		uart_state(p)->sensor.error = -ENOTSUP;
		uart_state(p)->sensor.fields = 0;
	}
	if (!data) return 0;
	uint32_t token = p->value.write.request + 1;
	if (!token) token++;
	uart_state(p)->write = (struct port_uart_write){.request = token, .mode = mode, .size = size};
	memcpy(uart_state(p)->write.data, data, size);
	p->value.write.request = token;
	p->value.write.error = -EAGAIN;
	return token;
}

/* Pybricks PFMotor single-output encoding, a7b1b04ae. Address bit zero
 * selects red/blue; the remaining two bits select one of four channels. */
static void pf_data(uint8_t output, uint8_t pwm, uint8_t data[2])
{
	data[0] = 0x40 | (output & 1) << 4 | pwm;
	data[1] = output >> 1;
}

/* Called by the port worker with lock held. A previous application write
 * is first cancelled/drained through the same UART state machine. */
static void pf_release(struct port *p)
{
	if (!p->motor.releasing || !pf_outputs(p) || pf_failure(p) || write_pending(p)) return;
	if (uart_state(p)->pf_generation != p->value.generation || p->value.phase != BP_PORT_READY ||
	    p->value.type_id != LEGO_DEVICE_TYPE_ID_COLOR_DIST_SENSOR) {
		uart_state(p)->pf_error = -ENODEV;
		return;
	}
	uint8_t output = 0;
	while (!(pf_outputs(p) & BIT(output))) output++;
	uint8_t data[2];
	pf_data(output, 0, data);
	uart_state(p)->pf_coasting = BIT(output);
	queue_write(p, LEGO_DEVICE_MODE_PUP_COLOR_DISTANCE_SENSOR__IR_TX, data, sizeof(data));
}

/* Caller holds lock. One port claim covers measurement selection and drive. */
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
static void stop_motor_log(struct motor_log *log, int error)
{
	if (log->status.active && !log->status.error) log->status.error = error;
	log->status.active = false;
}

static void stop_motor_logs(struct port *p, int error)
{
	for (size_t kind = 0; kind < BP_MOTOR_LOG_COUNT; kind++) stop_motor_log(&p->log[kind], error);
}

static void free_motor_log(struct motor_log *log)
{
	k_free(log->data);
	log->data = NULL;
	log->owner = BP_PORT_UNOWNED;
	log->status = (struct bp_motor_log_status){.revision = log->status.revision + 1};
}

/* Nonwaiting allocation under lock leaves no unpublished buffer when an
 * application is forcibly stopped. Only initialized rows become readable. */
static int start_motor_log(struct motor_log *log, enum bp_port_owner owner, uint32_t generation,
	uint32_t duration_ms, uint32_t down_sample, uint32_t columns)
{
	if (!down_sample) return -EINVAL;
	uint32_t rows = duration_ms / CONTROL_MS / down_sample;
	if (rows > SIZE_MAX / sizeof(int32_t) / columns) return -EOVERFLOW;
	int32_t *data = rows ? k_malloc(rows * columns * sizeof(*data)) : NULL;
	if (rows && !data) return -ENOMEM;
	k_free(log->data);
	log->data = data;
	log->owner = owner;
	log->generation = generation;
	log->down_sample = down_sample;
	log->skipped = 0;
	log->start_ms = k_uptime_get_32();
	log->status = (struct bp_motor_log_status){.revision = log->status.revision + 1,
		.capacity = rows, .columns = columns, .active = rows != 0};
	return 0;
}

static int read_motor_log(struct motor_log *log, uint32_t revision, uint32_t row, struct bp_motor_log_row *value)
{
	if (revision != log->status.revision) return -ECANCELED;
	if (row >= log->status.rows) return -ENOENT;
	*value = (struct bp_motor_log_row){0};
	memcpy(value->values, log->data + row * log->status.columns, log->status.columns * sizeof(int32_t));
	return 0;
}

static void free_idle_drivebase(size_t slot)
{
	struct drivebase *db = drivebases[slot];
	if (!db || db->owner) return;
	for (size_t axis = 0; axis < DRIVEBASE_AXES; axis++) if (db->log[axis].owner) return;
	k_free(db);
	drivebases[slot] = NULL;
}

static void free_motor_logs(enum bp_port_owner owner)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
		for (size_t kind = 0; kind < BP_MOTOR_LOG_COUNT; kind++) {
			struct motor_log *log = &ports[i].log[kind];
			if (log->owner != owner) continue;
			free_motor_log(log);
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(drivebases); i++) {
		struct drivebase *db = drivebases[i];
		if (!db) continue;
		for (size_t axis = 0; axis < DRIVEBASE_AXES; axis++) {
			struct motor_log *log = &db->log[axis];
			if (log->owner == owner) free_motor_log(log);
		}
		free_idle_drivebase(i);
	}
	k_spin_unlock(&lock, key);
}
#endif

static void drop_claim(struct port *p)
{
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	cancel_drivebase(p, -ENODEV, true);
	stop_motor_logs(p, 0);
#endif
	p->owner = BP_PORT_UNOWNED;
	p->motor.direction = 0;
	p->motor.max_voltage = 0;
	p->motor.gear_ratio = 0;
	p->motor.precision_profile = 0;
	p->motor.control.observing = false;
	p->value.selected_mode = UINT8_MAX;
	p->value.mode_error = -ENOTSUP;
	if (IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) {
		uart_state(p)->measurement = BP_SENSOR_DEFAULT;
		uart_state(p)->command++;
		uart_state(p)->sensor.error = -EAGAIN;
		uart_state(p)->sensor.fields = 0;
	}
	motor_activity(p - ports, false);
}

static void invalidate_motor(uint32_t port, int reason)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	cancel_drivebase(&ports[port], reason, true);
	stop_motor_logs(&ports[port], reason);
#endif
	finish_write(&ports[port], reason);
	if (pf_outputs(&ports[port]) && !pf_failure(&ports[port])) uart_state(&ports[port])->pf_error = reason;
	struct bp_motor_state *m = &ports[port].motor;
	m->request++;
	m->pending = false;
	m->releasing = true; /* No new claim until the worker finishes cleanup. */
	m->requested_mode = BP_MOTOR_COAST;
	m->requested_duty = 0;
	m->deadline_ms = 0;
	m->control.observing = false;
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
	p->motor.releasing = cleanup != 0 || pf_outputs(p);
	if (cleanup) {
		p->motor.valid = false;
		if (!p->motor.error) p->motor.error = cleanup;
		motor_activity(port, true);
	} else if (!reset_pending(p) && !pf_outputs(p) && !serial_mode(p)) {
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
		m->generation = ports[port].value.generation;
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
static int release_uart_pins(uint32_t port)
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
	return result;
}

static int release_pins(uint32_t port)
{
	int err = release_uart_pins(port);
	int cleanup = coast(port);
	return err ? err : cleanup;
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
	/* A request admitted during the preceding UART poll invalidates that
	 * poll's observation; only the next worker round may start resync.
	 * An I/O or cleanup failure still fails the accepted request explicitly. */
	if (IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART) &&
	    (serial_mode(p) || (uart_state(p)->reset_start && !error))) {
		k_spin_unlock(&lock, key);
		return;
	}
	struct bp_port_state v = uart ? *uart :
		(struct bp_port_state){.selected_mode = UINT8_MAX, .mode_error = -ENOTSUP};
	if (uart && write_pending(p) && uart->write.completed == p->value.write.request) {
		finish_write(p, uart->write.error);
	}
	v.write = p->value.write;
	if (IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART) && uart && uart_state(p)->command != command) {
		v.selected_mode = uart_state(p)->measurement & PORT_RAW_MODE ? uart_state(p)->measurement & 0xff : UINT8_MAX;
		v.mode_error = uart_state(p)->measurement & PORT_RAW_MODE ? -EAGAIN : -ENOTSUP;
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
	/* A warm UART device can present a stable empty/passive signature until
	 * its own keepalive expires. Only a new handshake or an error completes
	 * explicit reset; the overall deadline bounds continued detection. */
	if (reset_pending(p) && (phase == BP_PORT_READY || error)) {
		int result = error ? error : id != uart_state(p)->reset_type ? -ENODEV : 0;
		finish_resync(p, result, result ? 0 : v.generation);
		if (result && !cleanup) drop_claim(p);
		else if (!result && p->motor.valid) p->motor.error = 0;
	}
	v.reset = p->value.reset;
	v.failures = p->value.failures;
	v.sequence = p->value.sequence;
	if (error) {
		v.failures += v.failures != UINT32_MAX;
	} else {
		v.sequence += uart ? uart->data_sequence - p->value.data_sequence : 1;
	}
	p->value = v;
	if (IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) {
		if (sensor && uart_state(p)->command == command) {
			uart_state(p)->sensor = *sensor;
		} else if (!sensor) {
			uart_state(p)->sensor = (struct bp_sensor_sample){.error = v.error ? v.error :
				phase == BP_PORT_EMPTY ? -ENODEV : -ENOTSUP};
		}
		uart_state(p)->sensor.type_id = id;
		uart_state(p)->sensor.generation = v.generation;
	}
	k_spin_unlock(&lock, key);
}

#define TRY_IO(operation) do { int err_ = (operation); if (err_ < 0) { return err_; } } while (0)

/* Each return schedules at least 2 ms of settling. Reads never sleep or busy
 * wait, so six ports do not create six worker stacks or serialize their waits. */
static int advance(uint32_t port)
{
	struct port *p = &ports[port];
	int value;
	if (IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) {
		k_spinlock_key_t reset_key = k_spin_lock(&lock);
		bool restart = uart_state(p)->reset_start;
		uart_state(p)->reset_start = false;
		bool expired = reset_pending(p) && k_uptime_get() >= uart_state(p)->reset_deadline;
		k_spin_unlock(&lock, reset_key);
		if (expired) return -ETIMEDOUT;
		if (restart) {
			port_uart_stop(port);
			p->step = START;
		}
	}

	switch (p->step) {
	case UART: {
		if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
		struct bp_port_state uart = {0};
		struct bp_sensor_sample sensor;
		k_spinlock_key_t key = k_spin_lock(&lock);
		pf_release(p);
		uint32_t measurement = uart_state(p)->measurement, command = uart_state(p)->command;
		const struct port_uart_write *write = write_pending(p) ? &uart_state(&ports[port])->write : NULL;
		bool cancel = p->motor.releasing && !uart_state(p)->pf_coasting;
		k_spin_unlock(&lock, key);
		TRY_IO(port_uart_poll(port, &uart, &sensor, measurement, write, cancel));
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

/* Caller holds lock. A light command cannot energize a motor or vice versa. */
static int output_device_error(struct port *p, bool light)
{
	if (p->value.error) return p->value.error;
	if (k_uptime_get() - p->value.sample_ms > 10 * (SETTLE_MS + k_ticks_to_ms_ceil64(2))) return -ETIMEDOUT;
	if (p->value.phase == BP_PORT_EMPTY) return -ENODEV;
	if (p->value.phase == BP_PORT_PASSIVE &&
	    (light ? p->value.type_id == LEGO_DEVICE_TYPE_ID_LPF2_LIGHT :
	     p->value.type_id == LEGO_DEVICE_TYPE_ID_LPF2_MMOTOR ||
	     p->value.type_id == LEGO_DEVICE_TYPE_ID_LPF2_TRAIN)) return 0;
	if (light) return -ENOTSUP;
	if (p->value.phase == BP_PORT_NEGOTIATING || p->value.phase == BP_PORT_UART) return -EAGAIN;
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (p->value.phase != BP_PORT_READY || !(uart_state(p)->sensor.available & BIT(BP_SENSOR_ANGLE))) return -ENOTSUP;
	return uart_state(p)->sensor.error ? uart_state(p)->sensor.error :
		(uart_state(p)->sensor.fields & BP_SENSOR_HAS_ANGLE) ? 0 : -EAGAIN;
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

#ifdef CONFIG_BRICKPHYR_MOTORS
/* Caller holds lock. Never roll a successful PWM write back to its old drive:
 * an incomplete group always ends in coast, with the original cause retained. */
static void cancel_motor_group(int error)
{
	struct bp_motor_group_state *g = &motor_group.value;
	if (motor_group.cancelling) return;
	motor_group.cancelling = true;
	if (!g->error) g->error = error;
	g->active = false;
	g->pending = true;
	for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
		if (!(g->port_mask & BIT(i))) continue;
		if (!g->ports[i].error) g->ports[i].error = error == -ETIMEDOUT ? error : -ECANCELED;
		set_motor_request(&ports[i], BP_MOTOR_COAST, 0, 0);
	}
}

/* Called before each member's driver operation and after the final one. */
static void check_motor_group(void)
{
	struct bp_motor_group_state *g = &motor_group.value;
	if ((!g->active && !g->pending) || motor_group.cancelling) return;
	int error = atomic_get(&stopping) ? -ESHUTDOWN : 0;
	for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
		if (!(g->port_mask & BIT(i))) continue;
		const struct port *p = &ports[i];
		const struct bp_motor_state *m = &p->motor;
		int err = m->error ? m->error : p->value.generation != g->ports[i].generation ? -ESTALE :
			p->owner != motor_group.owner || m->request != g->ports[i].request ? -ECANCELED :
			output_device_error(&ports[i], false);
		if (!g->ports[i].error) g->ports[i].error = err;
		if (!error) error = err;
	}
	if (!error && g->active && k_uptime_get() >= g->deadline_ms) error = -ETIMEDOUT;
	if (error) cancel_motor_group(error);
}
#endif

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
/* Caller holds lock. An individual command cancels only active paired work;
 * association survives so the same DriveBase can be commanded again. */
static void cancel_drivebase(struct port *p, int error, bool detach)
{
	struct drivebase *db = p->drivebase;
	if (!db) return;
	if (db->value.active || db->value.pending || db->frame) {
		for (size_t i = 0; i < 2; i++) set_motor_request(db->motor[i], BP_MOTOR_COAST, 0, 0);
		db->cancelled = true;
		db->frame = false;
		db->value.command_error = error ? error : -ECANCELED;
		db->value.completed = db->value.request;
		db->value.pending = db->value.active = false;
		db->value.done = true;
	}
	if (detach) {
		db->detach = true;
		if (!db->value.error) db->value.error = error;
	}
	for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
		db->axis[i].trajectory = (struct bp_servo_trajectory){.done = true};
		if (detach || db->value.error) stop_motor_log(&db->log[i], error);
	}
}
#endif

static int request_motor(struct port *p, uint8_t mode, int32_t duty, uint32_t duration)
{
	if (group_member(p) && mode != BP_MOTOR_COAST) return -EBUSY;
	int err = k_work_reschedule_for_queue(PORT_QUEUE, &work, K_NO_WAIT);
	if (err < 0) return err;
#ifdef CONFIG_BRICKPHYR_MOTORS
	if (group_member(p)) {
		cancel_motor_group(-ECANCELED);
		return 0;
	}
#endif
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	cancel_drivebase(p, -ECANCELED, false);
#endif
	set_motor_request(p, mode, duty, duration);
	return 0;
}

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
/* Allocate before accepting a request; PBIO keeps pointers into this record.
 * Caller holds lock. Existing records survive replacement and in-flight work. */
static int request_control(struct port *p, uint8_t mode)
{
	bool created = !p->controller;
	if (created) {
		p->controller = k_malloc(sizeof(*p->controller));
		if (!p->controller) return -ENOMEM;
	}
	int err = request_motor(p, mode, 0, 0);
	if (err && created) {
		k_free(p->controller);
		p->controller = NULL;
	}
	return err;
}

static int control_input_error(struct port *p)
{
	if (!(uart_state(p)->sensor.fields & BP_SENSOR_HAS_ANGLE)) return -ENOTSUP;
	/* Without a new angle over the entire numerical derivative window, the
	 * reported zero speed would be indistinguishable from missing feedback. */
	return k_uptime_get() - uart_state(p)->sensor.sample_ms >=
		PBIO_CONFIG_DIFFERENTIATOR_WINDOW_SIZE * CONTROL_MS ? -ETIMEDOUT : 0;
}

/* Caller holds lock. The worker alone initializes or advances the model. */
static void start_observation(struct port *p)
{
	if (p->motor.control.observing) return;
	p->motor.control.observing = true;
	p->observer_reset = true;
	p->observer_rebase = false;
	/* All shafts sample on the same grid, including separately opened Motors.
	 * A paired controller must not advance either observer twice to align it. */
	p->control_due = (k_uptime_get() / CONTROL_MS + 1) * CONTROL_MS;
	p->observation.error = -EAGAIN;
}

/* The reference angle conversion rounds to output degrees. Divide before
 * rounding so accumulated shaft coordinates do not narrow to int32 mdeg. */
static int servo_angle(const struct bp_motor_state *motor, int64_t shaft, int32_t *value)
{
	int64_t angle;
	if (__builtin_sub_overflow(shaft, motor->zero_mdeg, &angle)) return -ERANGE;
	int64_t whole = angle / motor->gear_ratio, remainder = angle % motor->gear_ratio;
	if (llabs(remainder) * 2 >= motor->gear_ratio) whole += remainder < 0 ? -1 : 1;
	whole *= motor->direction;
	if (whole < INT32_MIN || whole > INT32_MAX) return -ERANGE;
	*value = whole;
	return 0;
}

/* Caller holds lock; rows contain only initialized firmware-owned values. */
static void append_log_row(struct motor_log *log, int32_t *values)
{
	values[0] = k_uptime_get_32() - log->start_ms;
	memcpy(log->data + log->status.rows++ * log->status.columns,
		values, log->status.columns * sizeof(int32_t));
	if (log->status.rows == log->status.capacity) log->status.active = false;
}

/* PBIO logger.c's bounded row append/downsampling, with explicit sample time
 * and firmware-owned memory. Caller holds lock; never copies a whole log. */
static void append_motor_log(struct port *p, uint32_t kind, uint32_t ticks,
	const struct motor_log_sample *s, const struct motor_pid_sample *pid)
{
	struct motor_log *log = &p->log[kind];
	if (!log->status.active || ++log->skipped != log->down_sample) return;
	log->skipped = 0;
	struct bp_motor_state *m = &p->motor;
	if (!m->gear_ratio || !m->direction) {
		stop_motor_log(log, -ENODEV);
		return;
	}
	struct bp_motor_log_row row;
	int err;
	if (kind == BP_MOTOR_LOG) {
		row = (struct bp_motor_log_row){.values = {0, ticks,
			0, s->speed / m->gear_ratio * m->direction, s->flags, s->voltage * m->direction,
			0, s->estimated_speed / m->gear_ratio * m->direction,
			s->feedback_torque * m->direction, s->feedforward_torque * m->direction, s->feedback_voltage * m->direction}};
		err = servo_angle(m, s->angle_mdeg, &row.values[2]);
		if (!err) err = servo_angle(m, s->estimated_angle, &row.values[6]);
	} else {
		memcpy(row.values, pid->values, sizeof(row.values));
		err = servo_angle(m, pid->values[2], &row.values[2]);
		if (!err) err = servo_angle(m, pid->values[6], &row.values[6]);
		if (!err) err = servo_angle(m, pid->values[8], &row.values[8]);
		row.values[3] = pid->values[3] / m->gear_ratio * m->direction;
		row.values[7] = pid->values[7] / m->gear_ratio * m->direction;
		row.values[9] = pid->values[9] / m->gear_ratio * m->direction;
		row.values[5] *= m->direction;
		for (size_t col = 10; col < BP_CONTROL_LOG_COLUMNS; col++) row.values[col] *= m->direction;
	}
	if (err) {
		stop_motor_log(log, err);
		return;
	}
	append_log_row(log, row.values);
}

static int control_frame(struct port *p, int64_t angle,
	const struct bp_motor_command *command, struct motor_reference *ref,
	int32_t *duty, int32_t *voltage, uint32_t ticks, int32_t max_voltage, struct motor_pid_sample *log,
	const struct drivebase_reference *paired, bool right, int direction, int64_t zero)
{
	struct bp_battery_sample battery;
	int err = hub_battery_get(&battery);
	if (!err) err = battery.error;
	if (err) return err;
	if (!battery.voltage_mv || battery.voltage_mv > 12000) return -ERANGE;
	if (paired) {
		drivebase_control_output(paired, p->controller, right, ref);
		int64_t position[2];
		for (size_t i = 0; i < 2; i++) {
			const pbio_angle_t *a = &paired->axis[i].position, *offset = &p->drivebase->controller.offset[i];
			position[i] = ((int64_t)a->rotations + offset->rotations) * 360000 + a->millidegrees + offset->millidegrees;
		}
		int64_t shaft = (position[0] + (right ? -position[1] : position[1])) * direction;
		if (__builtin_add_overflow(shaft, zero, &ref->angle_mdeg)) return -ERANGE;
		ref->speed_mdeg_s *= direction;
		ref->acceleration_mdeg_s2 *= direction;
		ref->voltage_mv *= direction;
		ref->feedback_torque *= direction;
		ref->feedforward_torque *= direction;
		pbio_angle_t unused_position;
		int32_t unused_speed;
		pbio_observer_get_estimated_state(&p->controller->observer, &unused_speed,
			&unused_position, &ref->estimated_speed);
	} else if (command) {
		/* Enum values are an explicit adapter mapping, not SDK PBIO types. */
		BUILD_ASSERT((int)BP_MOTOR_FINISH_COAST == (int)PBIO_CONTROL_ON_COMPLETION_COAST);
		BUILD_ASSERT((int)BP_MOTOR_FINISH_BRAKE == (int)PBIO_CONTROL_ON_COMPLETION_BRAKE);
		BUILD_ASSERT((int)BP_MOTOR_FINISH_HOLD == (int)PBIO_CONTROL_ON_COMPLETION_HOLD);
		BUILD_ASSERT((int)BP_MOTOR_FINISH_CONTINUE == (int)PBIO_CONTROL_ON_COMPLETION_CONTINUE);
		BUILD_ASSERT((int)BP_MOTOR_FINISH_COAST_SMART == (int)PBIO_CONTROL_ON_COMPLETION_COAST_SMART);
		BUILD_ASSERT((int)BP_MOTOR_FINISH_BRAKE_SMART == (int)PBIO_CONTROL_ON_COMPLETION_BRAKE_SMART);
		err = motor_control_command(p->controller, ticks, angle, command, battery.voltage_mv);
		if (err) return err;
	}
	if (!paired) err = motor_control_step(p->controller, ticks, angle, ref, log);
	if (err) return err;
	*duty = CLAMP((int64_t)CLAMP(ref->voltage_mv, -max_voltage, max_voltage) * 1000 / battery.voltage_mv, -1000, 1000);
	*voltage = (int64_t)*duty * battery.voltage_mv / 1000;
	return 0;
}

static int directed_state(struct port *p, int64_t angle, int64_t zero, int direction,
	pbio_control_state_t *value)
{
	int err = motor_control_state_get(p->controller, angle, value);
	if (err) return err;
	pbio_angle_t *positions[] = {&value->position, &value->position_estimate};
	for (size_t i = 0; i < ARRAY_SIZE(positions); i++) {
		int64_t raw = (int64_t)positions[i]->rotations * 360000 + positions[i]->millidegrees;
		if (__builtin_sub_overflow(raw, zero, &raw) ||
		    raw < -(int64_t)(INT32_MAX / 4) * 360000 || raw > (int64_t)(INT32_MAX / 4) * 360000) return -ERANGE;
		raw *= direction;
		*positions[i] = (pbio_angle_t){.rotations = raw / 360000, .millidegrees = raw % 360000};
	}
	if (value->speed == INT32_MIN || value->speed_estimate == INT32_MIN) return -ERANGE;
	value->speed *= direction;
	value->speed_estimate *= direction;
	return 0;
}

static int drivebase_gyro(struct drivebase *db, struct drivebase_state *state, bool check_epoch)
{
	struct bp_imu_application_state imu;
	int err = hub_imu_get_application(&imu);
	if (!err) err = imu.physical.error;
	if (!err && imu.pending) err = -EBUSY;
	if (!err && check_epoch && drivebase_control_active(&db->controller) &&
	    db->imu_epoch != imu.physical.epoch) err = -ESTALE;
	if (err) return err;
	int32_t scale = db->controller.control[DRIVEBASE_HEADING].settings.ctl_steps_per_app_step;
	/* PBIO imu.c's scaled heading, using the shared application's axes.
	 * Keep the motor-derived estimates for the controller's derivative term. */
	float rotations = imu.heading_3d / (360000.0f / scale);
	float speed = 0;
	for (size_t i = 0; i < 3; i++) speed -= imu.base[6 + i] * imu.physical.angular_velocity[i];
	speed *= scale / 1000.0f;
	if (!isfinite(rotations) || fabsf(rotations) >= INT32_MAX / 4 ||
	    !isfinite(speed) || fabsf(speed) >= INT32_MAX) return -ERANGE;
	pbio_control_state_t *heading = &state->axis[DRIVEBASE_HEADING];
	heading->position.rotations = (int32_t)rotations;
	heading->position.millidegrees = (int32_t)((imu.heading_3d -
		heading->position.rotations * (360000.0f / scale)) * scale);
	heading->speed = (int32_t)speed;
	db->imu_epoch = imu.physical.epoch;
	return 0;
}

static void prepare_drivebase(size_t slot, int64_t now)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct drivebase *db = drivebases[slot];
	if (!db || !db->owner) {
		k_spin_unlock(&lock, key);
		return;
	}
	db->frame = false;
	if (db->cancelled) {
		drivebase_control_stop(&db->controller);
		db->cancelled = false;
	}
	if (db->detach) {
		k_spin_unlock(&lock, key);
		return;
	}
	bool initializing = db->initializing;
	uint32_t request = db->value.request;
	bool pending = db->value.pending && db->applied != request;
	struct bp_drivebase_command command = db->command;
	int configure = db->configure;
	uint32_t settings_axis = db->settings_axis, settings_group = db->settings_group;
	struct bp_servo_settings axis_settings = db->axis_settings;
	struct motor_pid_sample samples[DRIVEBASE_AXES];
	struct motor_pid_sample *logs[DRIVEBASE_AXES];
	uint32_t log_revision[DRIVEBASE_AXES];
	for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
		logs[i] = db->log[i].status.active ? &samples[i] : NULL;
		log_revision[i] = db->log[i].status.revision;
	}
	bool gyro = db->value.gyro;
	bool gyro_change = pending && !configure && command.action == BP_DRIVEBASE_GYRO && command.gyro != gyro;
	struct bp_drivebase_settings settings = db->settings;
	/* Passive commands already use the ordinary output queue. Numerical
	 * work waits for both motors to reach the common observation grid. */
	if (db->motor[0]->motor.pending || db->motor[1]->motor.pending ||
	    now < db->motor[0]->control_due || now < db->motor[1]->control_due) {
		k_spin_unlock(&lock, key);
		return;
	}
	int err = db->value.error;
	int64_t angles[2], zeros[2];
	int directions[2];
	pbio_control_settings_t initial_settings[2];
	for (size_t i = 0; i < 2 && !err; i++) {
		struct port *p = db->motor[i];
		err = p->value.generation != db->setup.generations[i] || p->owner != db->owner ||
			!p->motor.control.observing ? -ENODEV : output_device_error(p, false);
		if (!err) err = p->motor.error ? p->motor.error : control_input_error(p);
		if (!err && (p->observer_reset || p->observer_rebase || !p->settings_ready ||
		    p->observation_revision != p->geometry_revision)) err = -EAGAIN;
		angles[i] = uart_state(p)->sensor.angle_mdeg;
		zeros[i] = p->motor.zero_mdeg;
		directions[i] = p->motor.direction;
		if (initializing) initial_settings[i] = p->control_settings;
	}
	int32_t scale = db->motor[0]->motor.gear_ratio;
	bool independently_stalled = db->motor[0]->observation.stalled || db->motor[1]->observation.stalled;
	k_spin_unlock(&lock, key);
	/* Rebase/setup is applied by each motor's ordinary frame first. */
	if (err == -EAGAIN) return;
	pbio_control_state_t wheels[2];
	for (size_t i = 0; i < 2 && !err; i++) err = directed_state(db->motor[i], angles[i], zeros[i], directions[i], &wheels[i]);
	if (!err && initializing) err = drivebase_control_init(&db->controller, &initial_settings[0], &initial_settings[1],
		scale, db->setup.wheel_um, db->setup.track_um, db->setup.spike);
	struct drivebase_state state = {0};
	if (!err) drivebase_control_state(&db->controller, &wheels[0], &wheels[1], &state);
	struct drivebase_state encoder_state = state;
	if (!err && gyro && !(gyro_change && !command.gyro)) err = drivebase_gyro(db, &state, true);
	int command_error = 0;
	bool reset = false, retry = false;
	if (!err && pending) {
		if (initializing || (!configure && command.action == BP_DRIVEBASE_RESET)) {
			int32_t distance = initializing ? 0 : command.distance, angle = initializing ? 0 : command.angle;
			command_error = drivebase_control_reset_check(&db->controller, &encoder_state, distance, angle);
			if (!command_error && gyro) command_error = hub_imu_try_heading(angle);
			if (command_error == -EAGAIN) retry = true;
			else if (!command_error) {
				command_error = drivebase_control_reset(&db->controller, &encoder_state, distance, angle);
				reset = !command_error;
				if (reset) {
					drivebase_control_state(&db->controller, &wheels[0], &wheels[1], &state);
					if (gyro) err = drivebase_gyro(db, &state, false);
				}
			}
		} else if (configure == DRIVE_CONFIG_BASIC) {
			command_error = drivebase_settings_set(&db->controller, &settings);
		} else if (configure == DRIVE_CONFIG_AXIS) {
			pbio_control_t *ctl = &db->controller.control[settings_axis];
			pbio_control_settings_t next = ctl->settings;
			command_error = motor_control_settings_set(&next, settings_group, &axis_settings);
			if (!command_error) controller_settings_apply(ctl, &next);
		} else if (command.action == BP_DRIVEBASE_GYRO) {
			if (gyro_change) {
				if (command.gyro) command_error = drivebase_gyro(db, &state, false);
				if (!command_error) {
					gyro = command.gyro;
					if (!gyro) state = encoder_state;
					drivebase_control_stop(&db->controller);
					reset = true;
				}
			}
		} else {
			struct drivebase_command move = {.distance = command.distance, .angle = command.angle,
				.speed = command.speed, .turn_rate = command.turn_rate, .duration_ms = command.duration_ms,
				.finish = command.finish, .motion = command.action == BP_DRIVEBASE_TIMED ? DRIVEBASE_TIMED :
				command.action == BP_DRIVEBASE_TURN ? DRIVEBASE_ABSOLUTE_TURN :
				command.action == BP_DRIVEBASE_VECTOR ? DRIVEBASE_VECTOR : DRIVEBASE_RELATIVE};
			if (command.action >= BP_DRIVEBASE_ARC_ANGLE && command.action <= BP_DRIVEBASE_CURVE) {
				command_error = drivebase_arc_command(command.radius,
					command.action == BP_DRIVEBASE_ARC_DISTANCE ? command.distance : command.angle,
					command.action == BP_DRIVEBASE_ARC_DISTANCE, command.action == BP_DRIVEBASE_CURVE, &move);
			} else if (command.action == BP_DRIVEBASE_TANK_ANGLE || command.action == BP_DRIVEBASE_TANK_TIME) {
				command_error = db->setup.spike ? drivebase_tank_command(command.speed, command.turn_rate,
					command.angle, command.action == BP_DRIVEBASE_TANK_TIME, &move) : -EINVAL;
			} else if (command.action == BP_DRIVEBASE_STEERING_ANGLE || command.action == BP_DRIVEBASE_STEERING_TIME) {
				int32_t left, right;
				command_error = db->setup.spike ? drivebase_steering_to_tank(command.speed, command.steering, &left, &right) : -EINVAL;
				if (!command_error) command_error = drivebase_tank_command(left, right, command.angle,
					command.action == BP_DRIVEBASE_STEERING_TIME, &move);
			}
			if (!command_error) command_error = drivebase_control_command(&db->controller, now * 10, &state, &move);
			if (!command_error) {
				for (size_t i = 0; i < 2; i++) pbio_control_stop(&db->motor[i]->controller->control);
			}
		}
	}
	bool active = !err && drivebase_control_active(&db->controller);
	struct drivebase_reference reference;
	if (active) err = drivebase_control_step(&db->controller, now * 10, &state, &reference, logs);
	struct bp_drivebase_state value = {0};
	if (!err) {
		int32_t distance_scale = db->controller.control[0].settings.ctl_steps_per_app_step;
		int32_t heading_scale = db->controller.control[1].settings.ctl_steps_per_app_step;
		int64_t distance = ((int64_t)state.axis[0].position.rotations * 360000 + state.axis[0].position.millidegrees) / distance_scale;
		int64_t heading = ((int64_t)state.axis[1].position.rotations * 360000 + state.axis[1].position.millidegrees) / heading_scale;
		if (distance <= INT32_MIN || distance >= INT32_MAX || heading <= INT32_MIN || heading >= INT32_MAX) err = -ERANGE;
		else value = (struct bp_drivebase_state){.sample_ms = now,
			.distance = pbio_control_settings_ctl_to_app_long(&db->controller.control[0].settings, &state.axis[0].position),
			.heading = pbio_control_settings_ctl_to_app_long(&db->controller.control[1].settings, &state.axis[1].position),
			.speed = state.axis[0].speed / distance_scale, .turn_rate = state.axis[1].speed / heading_scale,
			.angle = ((int64_t)state.axis[1].position.rotations * 360000 + state.axis[1].position.millidegrees) / (float)heading_scale,
			.active = drivebase_control_active(&db->controller), .done = drivebase_control_done(&db->controller),
			.stalled = active ? reference.stalled : independently_stalled, .valid = true, .gyro = gyro};
		drivebase_settings_get(&db->controller, &value.settings);
	}
	key = k_spin_lock(&lock);
	if (db->cancelled || db->detach) {
		k_spin_unlock(&lock, key);
		return;
	}
	if (err) {
		if (!db->value.error) db->value.error = err;
		db->value.valid = false;
		cancel_drivebase(db->motor[0], err, false);
	} else {
		value.request = db->value.request;
		value.completed = db->value.completed;
		value.motion = db->value.motion;
		value.pending = db->value.pending;
		value.command_error = db->value.command_error;
		db->value = value;
		for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
			pbio_control_t *ctl = &db->controller.control[i];
			struct bp_servo_settings *s = &db->axis[i].settings;
			if (initializing || (pending && !command_error && (configure == DRIVE_CONFIG_BASIC ||
			    (configure == DRIVE_CONFIG_AXIS && settings_axis == i)))) {
				/* Settings conversions need not repeat on every 5 ms frame. */
				motor_control_settings_get(&ctl->settings, ctl->settings.ctl_steps_per_app_step, s);
				s->revision = s->applied = request;
			}
			controller_trajectory_get(ctl, &db->axis[i].trajectory);
			struct motor_log *log = &db->log[i];
			if (!active || !logs[i] || log->status.revision != log_revision[i]) continue;
			if (samples[i].error) stop_motor_log(log, samples[i].error);
			else if (log->status.active && ++log->skipped == log->down_sample) {
				log->skipped = 0;
				append_log_row(log, samples[i].values);
			}
		}
		db->initializing = false;
		if (active) db->reference = reference;
		db->frame = active;
		if (pending && !retry) {
			db->applied = request;
			if (request == db->value.request) db->value.command_error = command_error;
			if (!configure && !command_error && (command.action != BP_DRIVEBASE_GYRO || gyro_change)) {
				db->value.motion = request;
				for (size_t i = 0; i < 2; i++) stop_motor_log(&db->motor[i]->log[BP_CONTROL_LOG], 0);
			}
		}
		if (reset) for (size_t i = 0; i < 2; i++) set_motor_request(db->motor[i], BP_MOTOR_COAST, 0, 0);
	}
	k_spin_unlock(&lock, key);
}
#endif

static __unused int64_t apply_motor(uint32_t port)
{
	struct port *p = &ports[port];
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bp_motor_state *m = &p->motor;
	int64_t expires = 0;
#ifdef CONFIG_BRICKPHYR_MOTORS
	if (group_member(p)) {
		check_motor_group();
		if (!motor_group.cancelling) expires = motor_group.value.deadline_ms;
	}
#endif
	int device_error = output_device_error(p, p->value.type_id == LEGO_DEVICE_TYPE_ID_LPF2_LIGHT);
	bool running = m->mode != BP_MOTOR_COAST && m->valid;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	bool observing = m->control.observing;
	if (!device_error && observing) device_error = control_input_error(p);
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
	bool paired = p->drivebase && p->drivebase->frame;
	struct drivebase_reference paired_reference;
	bool right = paired && p == p->drivebase->motor[1];
	int direction = m->direction;
	int64_t zero = m->zero_mdeg;
	if (paired) paired_reference = p->drivebase->reference;
	controlled |= paired;
	bool reset = observing && p->observer_reset;
	bool rebase = observing && p->observer_rebase;
	int64_t next_frame = observing ? p->control_due : 0;
	bool frame = observing && (!next_frame || k_uptime_get() >= next_frame || device_error);
	/* New trajectories join the existing sample cadence. Direct coast/brake
	 * and duty take effect immediately, without advancing the model early. */
	if (observing && !frame && (controlled || reset || rebase)) {
		k_spin_unlock(&lock, key);
		return next_frame;
	}
#else
	bool frame = false;
#endif
	if (!command_pending && !frame) {
		if (m->releasing && m->valid && m->mode == BP_MOTOR_COAST && !write_pending(p) && !pf_outputs(p)) {
			m->releasing = false;
			drop_claim(p);
		}
		int64_t deadline = m->deadline_ms;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
		if (observing && next_frame && (!deadline || next_frame < deadline)) deadline = next_frame;
#endif
		k_spin_unlock(&lock, key);
		return deadline;
	}
	uint32_t request = m->request, duration = p->motor_duration;
	uint32_t generation = p->value.generation;
	int32_t duty = command_pending ? m->requested_duty : m->duty;
	int err = mode != BP_MOTOR_COAST ? device_error ? device_error :
		p->motor_generation != p->value.generation ? -ENODEV : 0 : 0;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	struct bp_motor_command command = m->command;
	uint16_t type = p->value.type_id;
	int32_t profile = m->precision_profile;
	int32_t max_voltage = m->direction ? m->max_voltage : motor_max_voltage(type);
	uint32_t geometry_revision = p->geometry_revision;
	uint32_t settings_revision = p->settings_revision;
	bool settings_pending = frame && p->settings_ready && settings_revision != p->settings_applied;
	pbio_control_settings_t settings;
	pbio_observer_settings_t model_settings;
	if (settings_pending) {
		settings = p->control_settings;
		model_settings = p->model_settings;
	}
	bool logging[BP_MOTOR_LOG_COUNT];
	uint32_t log_revision[BP_MOTOR_LOG_COUNT];
	for (size_t kind = 0; kind < BP_MOTOR_LOG_COUNT; kind++) {
		logging[kind] = frame && p->log[kind].status.active && (kind == BP_MOTOR_LOG || (controlled && !paired));
		log_revision[kind] = p->log[kind].status.revision;
	}
	int64_t angle = uart_state(p)->sensor.angle_mdeg;
	if (frame && !err) err = device_error;
	bool overrun = frame && next_frame && k_uptime_get() >= next_frame + CONTROL_MS;
	if (overrun && !err) err = -ETIMEDOUT;
	if (frame) p->observer_reset = p->observer_rebase = false;
#endif
	bool output = command_pending || controlled;
	if (!err && output && (controlled || (mode == BP_MOTOR_DUTY && duty))) motor_activity(port, true);
	if (output) m->valid = false;
	k_spin_unlock(&lock, key);
	bool passive = mode == BP_MOTOR_COAST;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	struct motor_reference ref = {0};
	struct motor_observation observation = {0};
	struct motor_log_sample log_sample;
	struct motor_pid_sample pid_sample;
	int log_error = 0;
	int32_t voltage = 0;
	uint32_t ticks = k_ticks_to_us_floor64(k_uptime_ticks()) / 100;
	if (!err && frame) {
		next_frame = (next_frame ? next_frame : k_uptime_get()) + CONTROL_MS;
		if (reset) err = motor_control_init_profile(p->controller, type, angle, profile);
		else if (rebase) err = motor_control_reset_angle(p->controller, angle);
		if (!err && settings_pending) controller_settings_apply(&p->controller->control, &settings);
		if (!err && settings_pending) p->controller->observer.settings = model_settings;
	}
	if (observing && command_pending && !controlled && !err) {
		pbio_control_stop(&p->controller->control);
		p->controller->timed_hold = false;
	}
	if (!err && controlled) {
		err = control_frame(p, angle, command_pending ? &command : NULL,
			&ref, &duty, &voltage, ticks, max_voltage, logging[BP_CONTROL_LOG] ? &pid_sample : NULL,
			paired ? &paired_reference : NULL, right, direction, zero);
		passive = ref.coast;
	}
#endif
	bool coast_attempted = !err && output && passive;
	int output_error = 0;
	/* Computation/preemption must not turn an expired pending drive into a
	 * newly energized output. Expiry is also checked for the whole group. */
	if (!err && output && !passive && expires && k_uptime_get() >= expires) err = -ETIMEDOUT;
	if (!err && output) err = output_error = passive ? port_output_coast(port) : port_output_drive(port, duty);
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	if (!err && frame) {
		/* Prediction must use the output which actually reached the driver.
		 * Coast/brake need no battery sample. Unregulated PWM has the same
		 * cached voltage conversion as regulated output. */
		if (!controlled && !passive && duty) {
			struct bp_battery_sample battery;
			err = hub_battery_get(&battery);
			if (!err) err = battery.error;
			if (!err && (!battery.voltage_mv || battery.voltage_mv > 12000)) err = -ERANGE;
			if (!err) voltage = (int64_t)duty * battery.voltage_mv / 1000;
		}
		if (!err && logging[BP_MOTOR_LOG]) log_error = motor_control_log_sample(p->controller, ticks, angle,
			passive, voltage, &ref, &log_sample);
		if (!err) err = motor_control_predict(p->controller, ticks, angle, passive, voltage);
		if (!err) err = motor_control_observe(p->controller, ticks, angle, passive, &observation);
		if (logging[BP_MOTOR_LOG] || logging[BP_CONTROL_LOG]) {
			key = k_spin_lock(&lock);
			for (size_t kind = 0; kind < BP_MOTOR_LOG_COUNT; kind++) {
				if (!logging[kind] || p->log[kind].status.revision != log_revision[kind] ||
				    p->geometry_revision != geometry_revision) continue;
				int sample_error = err ? err : kind == BP_MOTOR_LOG ? log_error : pid_sample.error;
				if (sample_error) stop_motor_log(&p->log[kind], sample_error);
				else append_motor_log(p, kind, ticks, &log_sample, &pid_sample);
			}
			k_spin_unlock(&lock, key);
		}
		/* Include computation and driver time; never integrate catch-up steps. */
		if (k_uptime_get() >= next_frame) {
			overrun = true;
			if (!err) err = -ETIMEDOUT;
		}
	}
	p->control_due = !err && observing ? next_frame : 0;
	struct bp_servo_trajectory trajectory = {0};
	if (!err && observing && !paired && (frame || command_pending)) controller_trajectory_get(&p->controller->control, &trajectory);
#endif
	int cleanup = !err ? 0 : coast_attempted ? output_error : port_output_coast(port);
	key = k_spin_lock(&lock);
#ifdef CONFIG_BRICKPHYR_MOTORS
	if (group_member(p)) {
		/* Cancellation can arrive during the driver call. Record its result
		 * before the replacement coast can overwrite the port's completion. */
		struct bp_motor_group_state *g = &motor_group.value;
		if (request == g->ports[port].request && generation == g->ports[port].generation) {
			if (!err && output) g->applied_mask |= BIT(port);
			if (!g->ports[port].error) g->ports[port].error = err;
			if (!g->error) g->error = err;
		}
		if (!g->ports[port].cleanup_error) g->ports[port].cleanup_error = cleanup;
		if (!g->cleanup_error) g->cleanup_error = cleanup;
	}
#endif
	m->completed = request;
	m->pending = request != m->request;
	m->valid = !cleanup;
	m->cleanup_error = cleanup;
	if (output || err) m->deadline_ms = 0;
	if (output || err) m->control.active = false;
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
	if (!err && observing && (frame || command_pending)) p->trajectory = trajectory;
	if (observing && err) {
		stop_motor_logs(p, err);
		p->observation.error = err;
		m->control.observing = false;
	} else if (frame) {
		p->observation = (struct bp_motor_observation){.sample_ms = k_uptime_get(),
			.angle_mdeg = angle, .estimated_speed = observation.estimated_speed,
			.load_mnm = observation.load_mnm, .applied_mv = voltage,
			.generation = generation, .frames = p->observation.frames + 1,
			.stalled = observation.stalled};
		p->speed_history = p->controller->observer.differentiator;
		p->model_state = (struct bp_motor_model_state){.angle_mdeg = observation.estimated_angle,
			.speed_mdeg_s = observation.estimated_speed, .current_tenth_ma = observation.estimated_current,
			.stalled = observation.model_stalled};
		p->observation_revision = geometry_revision;
		if (geometry_revision == p->geometry_revision) {
			if (reset) {
				p->control_settings = p->controller->control.settings;
				p->model_settings = p->controller->observer.settings;
				p->settings_ready = true;
			}
			if (reset || settings_pending) p->settings_applied = settings_revision;
		}
	}
#endif
	if (err) {
		if (!m->error) m->error = err;
		m->failures += m->failures != UINT32_MAX;
	}
	if (m->valid && (output || err)) {
		m->mode = err || passive ? BP_MOTOR_COAST : controlled && !m->control.active ? BP_MOTOR_DUTY : mode;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
		if (paired && !err && !passive) m->mode = BP_MOTOR_DUTY;
#endif
		m->duty = err || passive ? 0 : duty;
		m->applied_ms = k_uptime_get();
		m->generation = generation;
		m->deadline_ms = !err && mode == BP_MOTOR_DUTY ? expires ? expires :
			duration ? m->applied_ms + duration : 0 : 0;
		p->supply = 0;
	}
	motor_activity(port, !m->valid || m->control.active || (m->mode != BP_MOTOR_COAST && m->duty));
	if (!m->pending && m->releasing && !err && mode == BP_MOTOR_COAST && !write_pending(p) && !pf_outputs(p)) {
		m->releasing = false;
		drop_claim(p);
	}
	int64_t deadline = m->deadline_ms;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	if (m->control.observing && p->control_due && (!deadline || p->control_due < deadline)) deadline = p->control_due;
#endif
	k_spin_unlock(&lock, key);
	return deadline;
}

#ifdef CONFIG_BRICKPHYR_MOTORS
/* Caller holds lock; also used after shutdown has directly released outputs. */
static void complete_motor_group(void)
{
	struct bp_motor_group_state *g = &motor_group.value;
	if (!g->pending) return;
	bool pending = false;
	for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
		if (!(g->port_mask & BIT(i))) continue;
		const struct bp_motor_state *m = &ports[i].motor;
		pending |= m->pending;
		if (!g->ports[i].cleanup_error) g->ports[i].cleanup_error = m->cleanup_error;
		if (!g->cleanup_error) g->cleanup_error = m->cleanup_error;
	}
	if (!pending) {
		g->pending = false;
		g->completed_ms = k_uptime_get();
	}
}

static void finish_motor_group(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bp_motor_group_state *g = &motor_group.value;
	check_motor_group();
	uint32_t coast = g->pending && motor_group.cancelling ? g->port_mask : 0;
	k_spin_unlock(&lock, key);
	/* Include earlier successful members when a later output failed. These
	 * calls use the ordinary driver/cleanup path in this same worker pass. */
	for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) if (coast & BIT(i)) apply_motor(i);
	key = k_spin_lock(&lock);
	complete_motor_group();
	k_spin_unlock(&lock, key);
}
#endif

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
static void finish_drivebase(size_t slot)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct drivebase *db = drivebases[slot];
	if (!db || !db->owner) {
		k_spin_unlock(&lock, key);
		return;
	}
	int err = 0;
	if (db->frame || (db->value.pending && db->applied == db->value.request)) {
		for (size_t i = 0; i < 2; i++) {
			const struct bp_motor_state *m = &db->motor[i]->motor;
			if (!err) err = m->error ? m->error : m->cleanup_error;
		}
		if (err) {
			if (!db->value.error) db->value.error = err;
			db->value.valid = false;
			cancel_drivebase(db->motor[0], err, false);
		}
	}
	bool cancelled = db->cancelled;
	k_spin_unlock(&lock, key);
	/* If either output failed, coast the other in this same worker pass. The
	 * ordinary path owns PWM, observer timing and first-error preservation.
	 * Concurrent single-Motor replacements remain queued on their own port. */
	if (cancelled) {
		for (size_t i = 0; i < 2; i++) apply_motor(db->motor[i] - ports);
	}
	key = k_spin_lock(&lock);
	bool pending = false;
	for (size_t i = 0; i < 2; i++) {
		const struct bp_motor_state *m = &db->motor[i]->motor;
		pending |= m->pending;
		if (!db->value.cleanup_error) db->value.cleanup_error = m->cleanup_error;
	}
	if (!pending) {
		if (db->value.pending && db->applied == db->value.request) {
			db->value.completed = db->value.request;
			db->value.pending = false;
		}
		if (db->detach) {
			for (size_t i = 0; i < 2; i++) db->motor[i]->drivebase = NULL;
			db->owner = BP_PORT_UNOWNED;
			free_idle_drivebase(slot);
		}
	}
	k_spin_unlock(&lock, key);
}
#endif

/* The worker owns pinmux and UART configuration. Reads are admitted only in
 * ACTIVE; marking CONFIGURE/CLOSE excludes them before touching the driver.
 * A raw peer has no identity/keepalive protocol: errors retain the lease until
 * explicit reconfiguration or release, never silently resume LEGO probing. */
static void serial_advance(uint32_t port)
{
	struct port *p = &ports[port];
	k_spinlock_key_t key = k_spin_lock(&lock);
	int action = serial_mode(p);
	uint32_t baud = uart_state(p)->serial_baud;
	uint8_t power_pin = uart_state(p)->serial_power;
	bool powered = p->value.baudrate && p->supply == power_pin && !p->value.cleanup_error;
	int err = 0, cleanup = 0;
	uint32_t completed = 0;
	if (action == SERIAL_ACTIVE) {
		/* TX admission and cancellation share this lock. The driver only
		 * copies a bounded packet and enables IRQs here; no configuration. */
		err = port_uart_serial_poll(port, write_pending(p) ? &uart_state(&ports[port])->write : NULL, &completed);
		if (err) {
			uart_state(p)->serial = action = SERIAL_FAILING;
			p->value.error = err;
			finish_write(p, err);
		} else if (write_pending(p) && completed == uart_state(&ports[port])->write.request) finish_write(p, 0);
	}
	k_spin_unlock(&lock, key);
	if (action == SERIAL_CONFIGURE || action == SERIAL_CLOSE) {
		port_uart_stop(port);
		/* Changing baud or clearing buffers must not power-cycle the peer.
		 * A new lease first releases any previous LEGO sensor supply. */
		err = action == SERIAL_CONFIGURE && powered ? release_uart_pins(port) : release_pins(port);
		if (!err && action == SERIAL_CONFIGURE) err = port_uart_serial_start(port, baud);
		if (!err && action == SERIAL_CONFIGURE) err = set_pin(port, BUFFER, GPIO_OUTPUT_ACTIVE);
		if (!err && action == SERIAL_CONFIGURE) err = sensor_supply(port, power_pin);
	}
	if (err) {
		port_uart_stop(port);
		cleanup = release_pins(port);
		if (action == SERIAL_CLOSE && !cleanup) cleanup = err;
	}
	key = k_spin_lock(&lock);
	p->value.sample_ms = k_uptime_get();
	p->value.sequence++;
	p->value.supply = p->supply;
	if (err) {
		if (!p->value.error || p->value.error == -EAGAIN) p->value.error = err;
		p->value.cleanup_error = p->motor.cleanup_error = cleanup;
		p->value.failures += p->value.failures != UINT32_MAX;
		finish_write(p, err);
		if (serial_mode(p) == action) uart_state(p)->serial = SERIAL_FAILED;
	} else if (action == SERIAL_CLOSE) {
		finish_write(p, -ECANCELED);
		uart_state(p)->serial = SERIAL_OFF;
		uart_state(p)->serial_power = 0;
		p->value.phase = BP_PORT_DETECTING;
		p->value.generation++;
		p->value.changed_ms = p->value.sample_ms;
		p->value.error = -EAGAIN;
		p->value.cleanup_error = 0;
		p->motor.releasing = false;
		drop_claim(p);
		p->step = START;
	} else if (action == SERIAL_CONFIGURE && serial_mode(p) == action) {
		uart_state(p)->serial = SERIAL_ACTIVE;
		p->value.error = p->value.cleanup_error = 0;
		p->value.baudrate = baud;
		motor_activity(port, power_pin != 0);
	}
	k_spin_unlock(&lock, key);
}

static void scan(struct k_work *item)
{
	int64_t next = INT64_MAX;
	for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
		if (!pins[i][P5].port) {
			continue;
		}
		int64_t started = k_uptime_get();
		if (started >= ports[i].next_ms) {
			k_spinlock_key_t key = k_spin_lock(&lock);
			bool serial = IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART) && serial_mode(&ports[i]) != SERIAL_OFF;
			bool first_failure = !ports[i].value.failures;
			k_spin_unlock(&lock, key);
			int err = 0;
			if (serial) serial_advance(i);
			else err = advance(i);
			if (err) {
				invalidate_motor(i, err);
				port_uart_stop(i);
				int cleanup = release_pins(i);
				finish_reset(i, cleanup);
				publish(i, err, cleanup, NULL, NULL, 0);
				if (first_failure) {
					LOG_WRN("Port %c failed at %lld ms: %d, cleanup %d, probe step %u, "
						"service late by %lld ms", 'A' + i, k_uptime_get(), err, cleanup,
						ports[i].step, started - ports[i].next_ms);
					if (ports[i].step == UART) port_uart_report_error(i);
				}
				ports[i].step = START;
			}
			ports[i].next_ms = k_uptime_get() + (err ? RETRY_MS : SETTLE_MS);
		}
		next = MIN(next, ports[i].next_ms);
	}
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	/* Acquire both encoders before computing a pair; neither motor advances
	 * its observer until the common reference has been calculated. */
	int64_t now = k_uptime_get();
	for (size_t i = 0; i < ARRAY_SIZE(drivebases); i++) prepare_drivebase(i, now);
#endif
#if defined(CONFIG_BRICKPHYR_MOTORS) || defined(CONFIG_BRICKPHYR_PORT_UART)
	for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
		if (!pins[i][P5].port) continue;
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool serial = IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART) && serial_mode(&ports[i]) != SERIAL_OFF;
		k_spin_unlock(&lock, key);
		int64_t deadline = serial ? 0 : apply_motor(i);
		if (deadline) next = MIN(next, deadline);
	}
#endif
#ifdef CONFIG_BRICKPHYR_MOTORS
	finish_motor_group();
#endif
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	for (size_t i = 0; i < ARRAY_SIZE(drivebases); i++) finish_drivebase(i);
	/* A coasting observation can be closed while computation runs outside
	 * lock. Retire only after both individual and paired work has finished. */
	k_spinlock_key_t key = k_spin_lock(&lock);
	for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
		struct port *p = &ports[i];
		if (!p->owner && p->controller) {
			k_free(p->controller);
			p->controller = NULL;
		}
	}
	k_spin_unlock(&lock, key);
#endif
	if (!atomic_get(&stopping) && next != INT64_MAX) {
		k_work_schedule_for_queue(PORT_QUEUE, &work,
			IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL) ? K_TIMEOUT_ABS_MS(next) :
			K_MSEC(MAX(1, next - k_uptime_get())));
	}
}

static int initialize(void)
{
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	k_work_queue_start(PORT_QUEUE, port_stack, K_KERNEL_STACK_SIZEOF(port_stack), 6, NULL);
#endif
	int err = gpio_is_ready_dt(&supply) ?
		gpio_pin_configure_dt(&supply, GPIO_OUTPUT_INACTIVE) : -ENODEV;
	for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
		ports[i].value.error = -EAGAIN;
		if (IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) {
			uart_state(&ports[i])->sensor.error = -EAGAIN;
			port_sensor_palette_default(&uart_state(&ports[i])->palette);
		}
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
		for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
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
	if (!port_present(port)) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	*value = ports[port].value;
	if (!value->cleanup_error) value->cleanup_error = pf_failure(&ports[port]);
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
	if (!port_present(port)) return -ENODEV;
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

static int raw_request(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t mode,
	const uint8_t *data, uint32_t size, uint32_t *request, uint8_t pf_output)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (port >= BP_PORT_COUNT || !owner_valid(owner)) {
		return -EINVAL;
	}
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = atomic_get(&stopping) ? -ESHUTDOWN :
		p->value.generation != generation ? -ESTALE :
		p->value.phase != BP_PORT_READY ? -ENODEV :
		pf_output && p->value.type_id != LEGO_DEVICE_TYPE_ID_COLOR_DIST_SENSOR ? -ENOTSUP :
		p->value.error && p->value.error != -EAGAIN ? p->value.error :
		k_uptime_get() - p->value.sample_ms > 10 * (SETTLE_MS + k_ticks_to_ms_ceil64(2)) ? -ETIMEDOUT :
		mode >= p->value.num_modes ? -EINVAL :
		(p->owner && p->owner != owner) || p->motor.pending || p->motor.releasing || write_pending(p) ||
		!p->motor.valid || p->motor.control.active || p->motor.mode != BP_MOTOR_COAST ? -EBUSY : 0;
	if (!err && data) {
		struct bp_port_mode info;
		err = port_uart_get_mode(port, mode, &info);
		if (!err && !info.output_mapping) err = -ENOTSUP;
		if (!err) {
			uint32_t width = info.data_type == LUMP_DATA_TYPE_DATA8 ? 1 :
				info.data_type == LUMP_DATA_TYPE_DATA16 ? 2 : 4;
			if (size != info.num_values * width) err = -EMSGSIZE;
		}
	}
	uint32_t token = 0;
	if (!err) {
		p->owner = owner;
		token = queue_write(p, mode, data, size);
		if (pf_output) {
			uart_state(p)->pf_used |= pf_output;
			uart_state(p)->pf_generation = generation;
			motor_activity(port, true);
		}
	}
	k_spin_unlock(&lock, key);
	if (!err && request) *request = token;
	return err;
}

int hub_port_select(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t mode)
{
	return raw_request(owner, port, generation, mode, NULL, 0, NULL, 0);
}

int hub_port_write(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t mode,
	const uint8_t *data, uint32_t size, uint32_t *request)
{
	if (!data || !request || !size || size > sizeof(uart_state(&ports[0])->write.data)) return -EINVAL;
	return raw_request(owner, port, generation, mode, data, size, request, 0);
}

int hub_port_pf_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const struct bp_pf_command *command, uint32_t *request)
{
	if (!command || !request || command->channel < 1 || command->channel > 4 ||
	    command->blue > 1 || command->brake > 1 || command->duty_permille < -1000 ||
	    command->duty_permille > 1000 || (command->brake && command->duty_permille)) return -EINVAL;
	int32_t duty = command->duty_permille;
	uint32_t magnitude = duty < 0 ? -duty : duty;
	/* Reference has seven speed steps: 15..99 percent truncate, 100 is 7. */
	uint8_t pwm = magnitude >= 1000 ? 7 : magnitude / 150;
	if (pwm && duty < 0) pwm = 16 - pwm;
	if (command->brake) pwm = 8;
	uint8_t output = (command->channel - 1) * 2 + command->blue, data[2];
	pf_data(output, pwm, data);
	return raw_request(owner, port, generation, LEGO_DEVICE_MODE_PUP_COLOR_DISTANCE_SENSOR__IR_TX,
		data, sizeof(data), request, BIT(output));
}

int hub_sensor_light_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const struct bp_sensor_light *light, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (!light || !request) return -EINVAL;
	struct bp_port_state state;
	int err = hub_port_get(port, &state);
	if (err) return err;
	if (state.generation != generation) return -ESTALE;
	uint8_t mode, data[4];
	uint32_t size;
	err = port_sensor_light_data(state.type_id, light, &mode, data, &size);
	if (err) return err;
	/* The shared writer rechecks generation and ownership before any I/O. */
	return hub_port_write(owner, port, generation, mode, data, size, request);
}

int hub_port_matrix_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const uint32_t *colors, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	struct bp_port_state state;
	int err = hub_port_get(port, &state);
	if (err) return err;
	if (state.generation != generation) return -ESTALE;
	if (state.type_id != LEGO_DEVICE_TYPE_ID_TECHNIC_COLOR_LIGHT_MATRIX) return -ENOTSUP;
	uint8_t data[9];
	err = port_matrix_data(colors, data);
	return err ? err : hub_port_write(owner, port, generation,
		LEGO_DEVICE_MODE_PUP_COLOR_LIGHT_MATRIX__PIX_O, data, sizeof(data), request);
}

int hub_port_reset(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (!request || port >= BP_PORT_COUNT ||
	    !owner_valid(owner)) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = atomic_get(&stopping) ? -ESHUTDOWN :
		p->value.generation != generation ? -ESTALE :
		reset_pending(p) || pf_outputs(p) ? -EBUSY : p->value.phase != BP_PORT_READY ? -ENODEV :
		p->value.error && p->value.error != -EAGAIN ? p->value.error :
		k_uptime_get() - p->value.sample_ms > 10 * (SETTLE_MS + k_ticks_to_ms_ceil64(2)) ? -ETIMEDOUT :
		(p->owner && p->owner != owner) || p->motor.pending || p->motor.releasing || write_pending(p) ||
		!p->motor.valid || p->motor.control.active || p->motor.mode != BP_MOTOR_COAST ? -EBUSY : 0;
	uint32_t token = 0;
	if (!err) {
		token = p->value.reset.request + 1;
		if (!token) token++;
		p->value.reset.request = token;
		p->value.reset.error = -EAGAIN;
		p->value.reset.generation = 0;
		uart_state(p)->reset_type = p->value.type_id;
		uart_state(p)->reset_start = true;
		/* Electrical debounce plus the existing bounded UART negotiation. */
		uart_state(p)->reset_deadline = k_uptime_get() + (MATCH_COUNT + 1) * 10 *
			(SETTLE_MS + k_ticks_to_ms_ceil64(2)) + PORT_UART_SYNC_TIMEOUT_MS;
		p->owner = owner;
		uart_state(p)->measurement = BP_SENSOR_DEFAULT;
		uart_state(p)->command++;
		p->value.generation++;
		p->value.phase = BP_PORT_NEGOTIATING;
		p->value.error = p->value.mode_error = -EAGAIN;
		p->value.selected_mode = UINT8_MAX;
		p->value.length = 0;
		uart_state(p)->sensor.generation = p->value.generation;
		uart_state(p)->sensor.error = -EAGAIN;
		uart_state(p)->sensor.fields = 0;
	}
	k_spin_unlock(&lock, key);
	if (!err) *request = token;
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
	if (!port_present(port)) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	*value = uart_state(&ports[port])->sensor;
	/* The port claim is authoritative for every sensor/motor view. */
	value->owner = ports[port].owner;
	struct bp_sensor_palette palette = uart_state(&ports[port])->palette;
	int64_t updated = ports[port].value.sample_ms;
	k_spin_unlock(&lock, key);
	if (!value->error && k_uptime_get() - updated > 10 * (SETTLE_MS + k_ticks_to_ms_ceil64(2))) {
		value->error = -ETIMEDOUT;
		value->fields = 0;
	}
	port_sensor_match(&palette, value);
	return 0;
}

/* Caller holds lock. Palettes only apply to the two supported color sensors. */
static int palette_device(const struct port *p, uint32_t generation)
{
	return atomic_get(&stopping) ? -ESHUTDOWN : p->value.generation != generation ? -ESTALE :
		p->value.phase != BP_PORT_READY ? -ENODEV :
		p->value.type_id != LEGO_DEVICE_TYPE_ID_SPIKE_COLOR_SENSOR &&
		p->value.type_id != LEGO_DEVICE_TYPE_ID_COLOR_DIST_SENSOR ? -ENOTSUP : 0;
}

int hub_sensor_palette_get(uint32_t port, uint32_t generation, struct bp_sensor_palette *palette)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (port >= BP_PORT_COUNT || !palette) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = palette_device(&ports[port], generation);
	if (!err) *palette = uart_state(&ports[port])->palette;
	k_spin_unlock(&lock, key);
	return err;
}

int hub_sensor_palette_set(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const struct bp_sensor_palette *palette)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (port >= BP_PORT_COUNT || !owner_valid(owner)) return -EINVAL;
	struct bp_sensor_palette next;
	if (palette) {
		next = *palette;
		int err = port_sensor_palette_check(&next);
		if (err) return err;
	} else port_sensor_palette_default(&next);
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = palette_device(p, generation);
	if (!err && (p->motor.releasing || (p->owner && p->owner != owner))) err = -EBUSY;
	if (!err) {
		uart_state(&ports[port])->palette = next;
		p->owner = owner;
	}
	k_spin_unlock(&lock, key);
	return err;
}

static int sensor_configure(enum bp_port_owner owner, uint32_t port, uint32_t measurement,
	const uint32_t *generation)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) {
		return -ENOTSUP;
	}
	if (port >= BP_PORT_COUNT || measurement >= BP_SENSOR_MEASUREMENT_COUNT ||
	    !owner_valid(owner)) {
		return -EINVAL;
	}
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = atomic_get(&stopping) ? -ESHUTDOWN :
		generation && p->value.generation != *generation ? -ESTALE :
		p->motor.releasing || write_pending(p) ? -EBUSY :
		p->value.phase != BP_PORT_READY ? -EAGAIN :
		!(uart_state(p)->sensor.available & BIT(measurement)) ? -ENOTSUP :
		p->owner && p->owner != owner ? -EBUSY : 0;
	if (!err) {
		p->owner = owner;
		if (uart_state(p)->measurement != measurement) {
			uart_state(p)->measurement = measurement;
			uart_state(p)->command++;
			uart_state(p)->sensor.measurement = measurement;
			p->value.selected_mode = UINT8_MAX;
			p->value.mode_error = -ENOTSUP;
			uart_state(p)->sensor.error = -EAGAIN;
			uart_state(p)->sensor.fields = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_sensor_configure(enum bp_port_owner owner, uint32_t port, uint32_t measurement)
{
	return sensor_configure(owner, port, measurement, NULL);
}

int hub_sensor_select(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t measurement)
{
	return sensor_configure(owner, port, measurement, &generation);
}

int hub_uart_open(enum bp_port_owner owner, uint32_t port, uint32_t baud, uint32_t power_pin, uint32_t *generation)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (port >= BP_PORT_COUNT || !baud || !generation || power_pin > 2 ||
	    !owner_valid(owner)) return -EINVAL;
	if (!port_uart_available(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = atomic_get(&stopping) ? -ESHUTDOWN :
		(p->owner && p->owner != owner) || serial_mode(p) || pf_outputs(p) || reset_pending(p) ||
		p->motor.pending || p->motor.releasing || write_pending(p) || !p->motor.valid ||
		p->motor.control.active || p->motor.mode != BP_MOTOR_COAST ? -EBUSY : 0;
#ifdef CONFIG_BRICKPHYR_PORT_UART
	if (!err && power_pin && !hub_uart_power_allowed()) err = -EACCES;
#endif
	if (!err) {
		err = k_work_reschedule_for_queue(PORT_QUEUE, &work, K_NO_WAIT);
		if (err >= 0) {
			uart_state(p)->serial = SERIAL_CONFIGURE;
			uart_state(p)->serial_baud = baud;
			uart_state(p)->serial_power = power_pin;
			p->owner = owner;
			uint32_t next = p->value.generation + 1;
			p->value = (struct bp_port_state){.generation = next, .phase = BP_PORT_SERIAL,
				.error = -EAGAIN, .sample_ms = k_uptime_get(), .changed_ms = k_uptime_get(),
				.selected_mode = UINT8_MAX, .mode_error = -ENOTSUP};
			uart_state(p)->sensor = (struct bp_sensor_sample){.generation = next, .error = -ENOTSUP};
			*generation = next;
			err = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

static int serial_access(struct port *p, enum bp_port_owner owner, uint32_t generation)
{
	return atomic_get(&stopping) ? -ESHUTDOWN : p->value.generation != generation ? -ESTALE :
		!serial_mode(p) ? -ENODEV : p->owner != owner ? -EBUSY :
		serial_mode(p) == SERIAL_CLOSE ? -ECANCELED : 0;
}

/* baud=0 is the private clear operation, retaining the selected baud. */
int hub_uart_configure(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t baud)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (port >= BP_PORT_COUNT) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = serial_access(p, owner, generation);
	if (!err && (serial_mode(p) == SERIAL_CONFIGURE || serial_mode(p) == SERIAL_FAILING)) err = -EBUSY;
	if (!err) {
		err = k_work_reschedule_for_queue(PORT_QUEUE, &work, K_NO_WAIT);
		if (err >= 0) {
			finish_write(p, -ECANCELED);
			if (baud) uart_state(p)->serial_baud = baud;
			uart_state(p)->serial = SERIAL_CONFIGURE;
			p->value.error = -EAGAIN;
			err = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_uart_read(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint8_t *data, uint32_t size)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (port >= BP_PORT_COUNT || size > 128 || (!data && size)) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = serial_access(p, owner, generation);
	if (!err) err = p->value.error;
	if (!err) err = data ? port_uart_serial_read(port, data, size) : port_uart_serial_waiting(port);
	k_spin_unlock(&lock, key);
	return err;
}

int hub_uart_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const uint8_t *data, uint32_t size, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	if (port >= BP_PORT_COUNT || !size || size > 32 || !data || !request) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = serial_access(p, owner, generation);
	if (!err) err = p->value.error;
	if (!err && write_pending(p)) err = -EBUSY;
	if (!err) *request = queue_write(p, 0, data, size);
	k_spin_unlock(&lock, key);
	return err;
}

static int port_release(enum bp_port_owner owner, uint32_t port, const uint32_t *generation)
{
	if (port >= BP_PORT_COUNT || !owner_valid(owner)) {
		return -EINVAL;
	}
	/* An absent port has never been claimed and its generation is zero. */
	if (!port_present(port)) return generation && *generation ? -ESTALE : 0;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = generation && p->value.generation != *generation ? -ESTALE :
		p->owner && p->owner != owner ? -EBUSY : 0;
	if (!err && p->owner && serial_mode(p)) {
		err = k_work_reschedule_for_queue(PORT_QUEUE, &work, K_NO_WAIT);
		if (err >= 0) {
			uart_state(p)->serial = SERIAL_CLOSE;
			if (!p->value.error || p->value.error == -EAGAIN) p->value.error = -ECANCELED;
			finish_write(p, -ECANCELED);
		}
		k_spin_unlock(&lock, key);
		return err < 0 ? err : 0;
	}
	if (!err && p->owner) {
		finish_resync(p, -ECANCELED, 0);
		/* A later cleanup attempt may use a reconnected transmitter solely
		 * to coast the remembered remote addresses. Old Python/native device
		 * objects remain bound to their original generation. Return the first
		 * failure even if this retry succeeds. */
		int pf_error = pf_failure(p);
		if (pf_outputs(p) && pf_error && p->value.phase == BP_PORT_READY &&
		    p->value.type_id == LEGO_DEVICE_TYPE_ID_COLOR_DIST_SENSOR &&
		    (!p->value.error || p->value.error == -EAGAIN) && !write_pending(p)) {
			uart_state(p)->pf_generation = p->value.generation;
			uart_state(p)->pf_error = 0;
		}
		/* Even passive observation can be computing outside lock. Keep its
		 * lease until the worker accepts coast, before admitting a new owner. */
		if (group_member(p) || p->motor.pending || p->motor.control.observing || p->motor.mode != BP_MOTOR_COAST || !p->motor.valid ||
		    p->motor.releasing || write_pending(p) || pf_outputs(p)) {
			p->motor.releasing = true;
			/* A sensor may need its existing DC supply to transmit IR. Local
			 * motor outputs are already passive; do not pulse sensor power. */
			bool passive = pf_outputs(p) && p->motor.valid && !p->motor.pending &&
				p->motor.mode == BP_MOTOR_COAST;
			err = atomic_get(&stopping) ? -ESHUTDOWN : passive ? 0 : request_motor(p, BP_MOTOR_COAST, 0, 0);
			if (err) {
				p->motor.cleanup_error = err;
				if (!p->motor.error) p->motor.error = err;
				if (pf_outputs(p) && !pf_failure(p)) uart_state(p)->pf_error = err;
			}
		} else {
			drop_claim(p);
		}
		if (pf_error) err = pf_error;
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_port_release(enum bp_port_owner owner, uint32_t port)
{
	return port_release(owner, port, NULL);
}

int hub_motor_close(enum bp_port_owner owner, uint32_t port, uint32_t generation)
{
	return port_release(owner, port, &generation);
}

void hub_ports_session_closed(enum bp_port_owner owner)
{
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	free_motor_logs(owner);
#endif
	for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
		hub_port_release(owner, i);
	}
}

int hub_ports_release(enum bp_port_owner owner)
{
	if (!owner_valid(owner)) return -EINVAL;
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
	free_motor_logs(owner);
#endif
	int result = 0;
	uint32_t previous_pf = 0;
	for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool owned = ports[i].owner == owner;
		if (owned) previous_pf += POPCOUNT(pf_outputs(&ports[i]));
		k_spin_unlock(&lock, key);
		int err = owned ? hub_port_release(owner, i) : 0;
		if (!result) result = err;
	}
	struct k_work_sync sync;
	for (;;) {
		bool pending = false, uart_pending = false;
		uint32_t remaining_pf = 0;
		k_spinlock_key_t key = k_spin_lock(&lock);
		for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
			/* A released lease can still have a frame in flight. Return its
			 * storage before application cleanup completes. */
			pending |= !ports[i].owner && ports[i].controller;
#endif
			if (ports[i].owner != owner) continue;
			remaining_pf += POPCOUNT(pf_outputs(&ports[i]));
			pending |= ports[i].motor.pending;
			uart_pending |= serial_mode(&ports[i]) == SERIAL_CLOSE || serial_mode(&ports[i]) == SERIAL_CONFIGURE;
			uart_pending |= write_pending(&ports[i]);
			uart_pending |= ports[i].motor.releasing && pf_outputs(&ports[i]) && !pf_failure(&ports[i]);
			if (!result) result = pf_failure(&ports[i]);
			if (!ports[i].motor.pending && !result) result = ports[i].motor.cleanup_error;
		}
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
		/* Drop even an idle pair before the next application can claim its
		 * ports; detached numerical state is retired by the worker. */
		for (size_t i = 0; i < ARRAY_SIZE(drivebases); i++) {
			pending |= drivebases[i] && drivebases[i]->owner == owner && drivebases[i]->detach;
		}
#endif
		k_spin_unlock(&lock, key);
		/* Eight outputs already need 2 s of settling, before selection and
		 * bounded UART waits. Feed only for a completed coast, never for a
		 * polling iteration. The watchdog accepts this only on the hub thread. */
		if (remaining_pf < previous_pf) hub_watchdog_feed();
		previous_pf = remaining_pf;
		if (!pending && !uart_pending) return result;
		k_work_flush_delayable(&work, &sync);
		/* Flushing can apply coast immediately, but cannot advance UART or
		 * settling deadlines. Yield until the next normal port round. */
		if (uart_pending) k_sleep(K_MSEC(SETTLE_MS));
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
	if (!value->error) value->error = atomic_get(&stopping) ? -ESHUTDOWN :
		output_device_error(&ports[port], ports[port].value.type_id == LEGO_DEVICE_TYPE_ID_LPF2_LIGHT);
	k_spin_unlock(&lock, key);
	return 0;
}

int hub_control_port_get(uint32_t port, struct bp_control_port *value)
{
	if (!value || port >= BP_PORT_COUNT) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	int output_error = port_output_check(port);
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	const struct bp_motor_state *motor = &p->motor;
	*value = (struct bp_control_port){
		.generation = p->value.generation,
		.requested_ms = motor->requested_ms, .applied_ms = motor->applied_ms,
		.request = motor->request, .completed = motor->completed,
		.request_generation = p->motor_generation, .output_generation = motor->generation,
		.output_error = output_error ? output_error : motor->error,
		.cleanup_error = motor->cleanup_error, .duty_permille = motor->duty,
		.mode = motor->mode, .valid = motor->valid && motor->generation == p->value.generation,
		.pending = motor->pending,
		.owner = p->owner,
	};
	/* Reuse the same device validity as direct output. Passive motors may
	 * have valid output without an encoder; the two errors stay independent. */
	int err = atomic_get(&stopping) ? -ESHUTDOWN : output_device_error(p, false);
	if (!value->output_error) value->output_error = err;
	if (!value->output_error && motor->generation != p->value.generation) value->output_error = -ESTALE;
	value->error = err ? err : -ENOTSUP;
	if (IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) {
		const struct bp_sensor_sample *sensor = &uart_state(p)->sensor;
		value->sample_ms = sensor->sample_ms;
		value->angle_mdeg = sensor->angle_mdeg;
		value->sequence = sensor->sequence;
		value->error = err ? err : sensor->error ? sensor->error :
			!(sensor->fields & BP_SENSOR_HAS_ANGLE) ? -ENOTSUP : 0;
	}
	k_spin_unlock(&lock, key);
	return 0;
}

/* Connection and configuration checks share the command lock, so a disconnect
 * or another resource owner cannot slip between validation and queueing. */
static int motor_connection_error(const struct port *p, enum bp_port_owner owner, uint32_t generation)
{
	return atomic_get(&stopping) ? -ESHUTDOWN : p->value.generation != generation ? -ESTALE :
		serial_mode(p) || p->motor.releasing || write_pending(p) || pf_outputs(p) ||
		(p->owner && p->owner != owner) ? -EBUSY : 0;
}

int hub_motor_group_exchange(enum bp_port_owner owner, const struct bp_motor_group_command *command,
	uint32_t *request, struct bp_motor_group_state *previous)
{
	if (previous) *previous = (struct bp_motor_group_state){0};
#ifndef CONFIG_BRICKPHYR_MOTORS
	return -ENOTSUP;
#else
	if (!command || !request || !command->port_mask || command->port_mask >> BP_PORT_COUNT ||
	    command->coast_mask & ~command->port_mask || command->deadline_ms < 0 ||
	    !owner_valid(owner)) return -EINVAL;
	bool active = command->coast_mask != command->port_mask;
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!(command->port_mask & BIT(i))) continue;
		int32_t duty = command->ports[i].duty_permille;
		if (duty < -1000 || duty > 1000 || ((command->coast_mask & BIT(i)) && duty)) return -EINVAL;
		int err = port_output_check(i);
		if (err) return err;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bp_motor_group_state *g = &motor_group.value;
	/* Preserve the old result under the same lock as replacement. A separate
	 * get/set pair can miss completion between those calls. */
	if (previous) *previous = *g;
	int64_t now = k_uptime_get();
	int err = atomic_get(&stopping) ? -ESHUTDOWN :
		active && command->deadline_ms <= now ? -ETIMEDOUT :
		g->active && g->deadline_ms <= now ? -ETIMEDOUT :
		g->request == UINT32_MAX ? -EOVERFLOW :
		g->pending || (g->active && (motor_group.owner != owner || g->port_mask != command->port_mask)) ? -EBUSY : 0;
	for (uint32_t i = 0; i < ARRAY_SIZE(ports) && !err; i++) {
		if (!(command->port_mask & BIT(i))) continue;
		struct port *p = &ports[i];
		err = motor_connection_error(p, owner, command->ports[i].generation);
		if (!err) err = p->motor.pending || p->motor.control.observing ? -EBUSY : output_device_error(p, false);
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
		if (!err && p->drivebase) err = -EBUSY;
#endif
		if (!err && p->motor.cleanup_error) err = p->motor.cleanup_error;
	}
	/* Scheduling precedes publication; any rejection above has no side effects. */
	if (!err) {
		err = k_work_reschedule_for_queue(PORT_QUEUE, &work, K_NO_WAIT);
		if (err >= 0) {
			*g = (struct bp_motor_group_state){.requested_ms = now, .deadline_ms = active ? command->deadline_ms : 0,
				.request = g->request + 1, .port_mask = command->port_mask, .pending = true, .active = active};
			motor_group.owner = owner;
			motor_group.cancelling = false;
			for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
				if (!(g->port_mask & BIT(i))) continue;
				struct port *p = &ports[i];
				set_motor_request(p, command->coast_mask & BIT(i) ? BP_MOTOR_COAST : BP_MOTOR_DUTY,
					command->ports[i].duty_permille, 0);
				p->owner = owner;
				p->motor.requested_ms = now;
				p->motor.error = p->motor.cleanup_error = 0;
				g->ports[i].generation = command->ports[i].generation;
				g->ports[i].request = p->motor.request;
			}
			*request = g->request;
			err = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
#endif
}

int hub_motor_group_set(enum bp_port_owner owner, const struct bp_motor_group_command *command, uint32_t *request)
{
	return hub_motor_group_exchange(owner, command, request, NULL);
}

int hub_motor_group_get(struct bp_motor_group_state *state)
{
#ifndef CONFIG_BRICKPHYR_MOTORS
	return -ENOTSUP;
#else
	if (!state) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	*state = motor_group.value;
	k_spin_unlock(&lock, key);
	return state->port_mask ? 0 : -ENODATA;
#endif
}

int hub_motor_setup(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	int32_t direction, uint32_t *request)
{
	if (port >= BP_PORT_COUNT || !request || (direction != 1 && direction != -1) ||
	    !owner_valid(owner)) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	err = motor_connection_error(p, owner, generation);
	if (!err) err = p->motor.pending || p->motor.control.active ? -EBUSY : output_device_error(p, false);
	if (!err) err = request_motor(p, BP_MOTOR_COAST, 0, 0);
	if (!err) {
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
		cancel_drivebase(p, -ENODEV, true);
#endif
		p->owner = owner;
		p->motor.direction = direction;
		p->motor.gear_ratio = 0;
		p->motor.precision_profile = 0;
		p->motor.max_voltage = motor_max_voltage(p->value.type_id);
		p->motor.error = p->motor.cleanup_error = 0;
		*request = p->motor.request;
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_motor_observe(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t *request)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || !request ||
	    !owner_valid(owner)) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	err = motor_connection_error(p, owner, generation);
	if (!err) err = p->motor.pending ? -EBUSY : output_device_error(p, false);
	if (!err) err = control_input_error(p);
	struct bp_motor_command probe = {.mode = BP_MOTOR_SPEED};
	if (!err) err = motor_control_check(p->value.type_id, uart_state(p)->sensor.angle_mdeg, &probe);
	if (!err) err = request_control(p, BP_MOTOR_COAST);
	if (!err) {
		p->owner = owner;
		p->motor.error = p->motor.cleanup_error = 0;
		start_observation(p);
		*request = p->motor.request;
	}
	k_spin_unlock(&lock, key);
	return err;
#endif
}

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
static bool speed_window_valid(uint32_t window_ms)
{
	/* PBIO narrows the rounded sample count to uint8_t. Check it first. */
	uint64_t samples = ((uint64_t)window_ms + CONTROL_MS / 2) / CONTROL_MS;
	return samples && samples < PBIO_CONFIG_DIFFERENTIATOR_BUFFER_SIZE;
}

/* Caller holds lock; the publication and history belong to one model frame. */
static int observation_snapshot(struct port *p, uint32_t generation,
	struct bp_motor_observation *value, pbio_differentiator_t *history)
{
	*value = p->observation;
	value->error = p->value.generation != generation ? -ESTALE :
		p->motor.error ? p->motor.error : !p->motor.control.observing ? -ENODEV : value->error;
	if (!value->error) value->error = output_device_error(p, false);
	if (!value->error) value->error = control_input_error(p);
	if (!value->error && k_uptime_get() - value->sample_ms >= 2 * CONTROL_MS) value->error = -ETIMEDOUT;
	if (!value->error) *history = p->speed_history;
	return value->error;
}
#endif

int hub_motor_observation_get(uint32_t port, uint32_t generation, uint32_t window_ms,
	struct bp_motor_observation *value)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (!value || port >= BP_PORT_COUNT || !speed_window_valid(window_ms)) return -EINVAL;
	pbio_differentiator_t history;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	observation_snapshot(&ports[port], generation, value, &history);
	k_spin_unlock(&lock, key);
	if (!value->error && pbio_differentiator_get_speed(&history, window_ms, &value->speed_mdeg_s)) return -EINVAL;
	return 0;
#endif
}

int hub_servo_setup(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const struct bp_servo_setup *setup, uint32_t *request)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || !setup || !request ||
	    !owner_valid(owner) ||
	    (setup->direction != 1 && setup->direction != -1) || setup->gear_ratio < 1 ||
	    setup->precision_profile < 0 || setup->precision_profile > INT32_MAX / 1000 ||
	    setup->reset_angle > 1) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	err = motor_connection_error(p, owner, generation);
	if (!err) err = p->motor.pending ? -EBUSY : output_device_error(p, false);
	if (!err) err = control_input_error(p);
	struct bp_motor_command probe = {.mode = BP_MOTOR_SPEED};
	if (!err) err = motor_control_check(p->value.type_id, uart_state(p)->sensor.angle_mdeg, &probe);
	int64_t zero = generation == p->zero_generation ? p->motor.zero_mdeg : 0;
	if (!err && setup->reset_angle) {
		int32_t absolute = uart_state(p)->sensor.fields & BP_SENSOR_HAS_ABSOLUTE_ANGLE ? uart_state(p)->sensor.absolute_mdeg : 0;
		if (__builtin_sub_overflow(uart_state(p)->sensor.angle_mdeg, absolute, &zero)) err = -ERANGE;
	}
	if (!err) err = request_control(p, BP_MOTOR_COAST);
	if (!err) {
		cancel_drivebase(p, -ENODEV, true);
		p->owner = owner;
		p->motor.direction = setup->direction;
		p->motor.gear_ratio = setup->gear_ratio;
		p->motor.precision_profile = setup->precision_profile;
		p->motor.zero_mdeg = zero;
		p->geometry_revision++;
		p->zero_generation = generation;
		p->motor.max_voltage = motor_max_voltage(p->value.type_id);
		p->motor.error = p->motor.cleanup_error = 0;
		start_observation(p);
		/* A new constructor resets settings/model, even on an existing lease. */
		p->observer_reset = true;
		p->settings_ready = false;
		p->settings_revision++;
		p->observation.error = -EAGAIN;
		*request = p->motor.request;
	}
	k_spin_unlock(&lock, key);
	return err;
#endif
}

int hub_servo_get(uint32_t port, uint32_t generation, uint32_t window_ms, struct bp_servo_state *value)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || !value || !speed_window_valid(window_ms)) return -EINVAL;
	struct bp_motor_observation observation;
	pbio_differentiator_t history;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = observation_snapshot(p, generation, &observation, &history);
	/* An in-flight old frame may publish after reset/setup was accepted. Do
	 * not combine its history with the new coordinates before their frame. */
	if (!err && p->observation_revision != p->geometry_revision) err = -EAGAIN;
	struct bp_motor_state motor = p->motor;
	if (!err && (!motor.gear_ratio || !motor.direction || !p->owner)) err = -ENODEV;
	k_spin_unlock(&lock, key);
	if (err) return err;
	int32_t speed;
	if (pbio_differentiator_get_speed(&history, window_ms, &speed)) return -EINVAL;
	int32_t angle;
	err = servo_angle(&motor, observation.angle_mdeg, &angle);
	if (err) return err;
	*value = (struct bp_servo_state){.sample_ms = observation.sample_ms, .angle = angle,
		.speed = speed / motor.gear_ratio * motor.direction, .load = observation.load_mnm * motor.direction,
		.request = motor.request, .completed = motor.completed, .pending = motor.pending,
		.active = motor.control.active, .done = !motor.pending && (!motor.control.active || motor.control.done),
		.stalled = observation.stalled};
	return 0;
#endif
}

int hub_servo_reset(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	int32_t angle, uint32_t absolute, uint32_t *request)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || !request || absolute > 1 ||
	    !owner_valid(owner)) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	struct bp_motor_state *m = &p->motor;
	int err = motor_connection_error(p, owner, generation);
	if (!err) err = p->owner != owner || !m->gear_ratio || !m->control.observing ? -ENODEV :
		m->pending ? -EBUSY : m->error ? m->error : output_device_error(p, false);
	if (!err) err = control_input_error(p);
	if (!err && absolute && !(uart_state(p)->sensor.fields & BP_SENSOR_HAS_ABSOLUTE_ANGLE)) err = -ENOTSUP;
	int64_t zero = 0;
	int64_t shaft = absolute ? uart_state(p)->sensor.absolute_mdeg : (int64_t)angle * m->gear_ratio * m->direction;
	if (!err && __builtin_sub_overflow(uart_state(p)->sensor.angle_mdeg, shaft, &zero)) err = -ERANGE;
	/* Resetting an active trajectory applies its completion action at the new
	 * zero; a direct voltage/brake/coast command keeps its current output. */
	uint8_t mode = m->mode;
	int32_t duty = m->duty;
	uint32_t duration = m->deadline_ms ? MAX(m->deadline_ms - k_uptime_get(), 1) : 0;
	if (m->control.active) {
		mode = m->command.finish == BP_MOTOR_FINISH_COAST || m->command.finish == BP_MOTOR_FINISH_COAST_SMART ? BP_MOTOR_COAST :
			m->command.finish == BP_MOTOR_FINISH_BRAKE || m->command.finish == BP_MOTOR_FINISH_BRAKE_SMART ? BP_MOTOR_DUTY : BP_MOTOR_POSITION;
		duty = 0;
		duration = 0;
	}
	if (!err) err = request_motor(p, mode, duty, duration);
	if (!err) {
		if (mode == BP_MOTOR_POSITION) m->command = (struct bp_motor_command){
			.mode = mode, .target_mdeg = uart_state(p)->sensor.angle_mdeg, .finish = BP_MOTOR_FINISH_HOLD};
		m->zero_mdeg = zero;
		p->geometry_revision++;
		p->observer_rebase = true;
		p->observation.error = -EAGAIN;
		*request = m->request;
	}
	k_spin_unlock(&lock, key);
	return err;
#endif
}

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
static int servo_control_error(struct port *p, uint32_t generation)
{
	if (p->value.generation != generation || !p->owner || !p->motor.gear_ratio) return -ENODEV;
	if (p->motor.error) return p->motor.error;
	if (!p->motor.control.observing) return -ENODEV;
	if (!p->settings_ready || p->observation_revision != p->geometry_revision) return -EAGAIN;
	return control_input_error(p);
}

static struct drivebase *find_drivebase(enum bp_port_owner owner, uint32_t handle)
{
	if (!handle || !owner_valid(owner)) return NULL;
	for (size_t i = 0; i < ARRAY_SIZE(drivebases); i++) {
		struct drivebase *db = drivebases[i];
		if (db && db->handle == handle && db->owner == owner) return db;
	}
	return NULL;
}

int hub_drivebase_open(enum bp_port_owner owner, const struct bp_drivebase_setup *setup, uint32_t *handle)
{
	if (!setup || !handle || setup->ports[0] >= BP_PORT_COUNT || setup->ports[1] >= BP_PORT_COUNT ||
	    setup->ports[0] == setup->ports[1] || setup->spike > 1 ||
	    !owner_valid(owner)) return -EINVAL;
	if (!port_present(setup->ports[0]) || !port_present(setup->ports[1])) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p[2] = {&ports[setup->ports[0]], &ports[setup->ports[1]]};
	int err = atomic_get(&stopping) ? -ESHUTDOWN : 0;
	for (size_t i = 0; i < 2 && !err; i++) {
		err = servo_control_error(p[i], setup->generations[i]);
		if (!err && (p[i]->owner != owner || p[i]->drivebase || p[i]->motor.pending || p[i]->motor.releasing || group_member(p[i]))) err = -EBUSY;
	}
	if (!err && p[0]->motor.gear_ratio != p[1]->motor.gear_ratio) err = -EINVAL;
	int32_t scales[2];
	if (!err) err = drivebase_control_scales(p[0]->motor.gear_ratio, setup->wheel_um, setup->track_um, setup->spike, scales);
	size_t slot;
	for (slot = 0; slot < ARRAY_SIZE(drivebases); slot++) {
		if (!drivebases[slot] || !drivebases[slot]->owner) break;
	}
	if (!err && slot == ARRAY_SIZE(drivebases)) err = -ENOSPC;
	if (!err && drivebase_handle == UINT32_MAX) err = -EOVERFLOW;
	if (!err) {
		struct drivebase *db = drivebases[slot];
		bool fresh = !db;
		/* Nonwaiting allocation precedes every output/ownership change. The
		 * lock prevents an aborted caller from leaving an unpublished record. */
		if (fresh) db = k_malloc(sizeof(*db));
		if (!db) {
			k_spin_unlock(&lock, key);
			return -ENOMEM;
		}
		err = k_work_reschedule_for_queue(PORT_QUEUE, &work, K_NO_WAIT);
		if (err >= 0) {
			if (!fresh) for (size_t i = 0; i < DRIVEBASE_AXES; i++) free_motor_log(&db->log[i]);
			*db = (struct drivebase){.motor = {p[0], p[1]}, .owner = owner, .setup = *setup,
				.handle = ++drivebase_handle, .initializing = true,
				.value = {.request = 1, .pending = true}};
			drivebases[slot] = db;
			for (size_t i = 0; i < 2; i++) {
				set_motor_request(p[i], BP_MOTOR_COAST, 0, 0);
				p[i]->drivebase = db;
				stop_motor_log(&p[i]->log[BP_CONTROL_LOG], 0);
			}
			*handle = db->handle;
			err = 0;
		} else if (fresh) k_free(db);
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_drivebase_get(enum bp_port_owner owner, uint32_t handle, struct bp_drivebase_state *value)
{
	if (!value) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct drivebase *db = find_drivebase(owner, handle);
	if (db) {
		*value = db->value;
		if (value->valid && !value->error && k_uptime_get() - value->sample_ms >= 2 * CONTROL_MS) {
			value->error = -ETIMEDOUT;
			value->valid = false;
		}
	}
	k_spin_unlock(&lock, key);
	return db ? 0 : -ENODEV;
}

int hub_drivebase_command(enum bp_port_owner owner, uint32_t handle,
	const struct bp_drivebase_command *command, uint32_t *request)
{
	if (!command || !request || command->action > BP_DRIVEBASE_VECTOR ||
	    command->finish > BP_MOTOR_FINISH_BRAKE_SMART || command->gyro > 1) return -EINVAL;
	if (command->action == BP_DRIVEBASE_GYRO && command->gyro && !IS_ENABLED(CONFIG_BRICKPHYR_IMU)) return -ENOTSUP;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct drivebase *db = find_drivebase(owner, handle);
	int err = atomic_get(&stopping) ? -ESHUTDOWN : !db || db->detach ? -ENODEV : db->initializing ||
		(db->value.pending && command->action != BP_DRIVEBASE_COAST && command->action != BP_DRIVEBASE_BRAKE) ? -EBUSY : 0;
	if (!err && db->value.error) err = db->value.error;
	if (!err) {
		err = k_work_reschedule_for_queue(PORT_QUEUE, &work, K_NO_WAIT);
		if (err >= 0) {
			db->command = *command;
			db->configure = DRIVE_CONFIG_NONE;
			db->value.command_error = 0;
			db->value.pending = true;
			*request = ++db->value.request;
			if (command->action == BP_DRIVEBASE_COAST || command->action == BP_DRIVEBASE_BRAKE) {
				/* Cancel a calculation already outside the lock before it can
				 * publish another paired output after this stop was accepted. */
				db->cancelled = true;
				db->frame = false;
				db->applied = *request;
				db->value.motion = *request;
				db->value.active = false;
				db->value.done = true;
			for (size_t i = 0; i < DRIVEBASE_AXES; i++) db->axis[i].trajectory = (struct bp_servo_trajectory){.done = true};
				for (size_t i = 0; i < 2; i++) set_motor_request(db->motor[i],
					command->action == BP_DRIVEBASE_COAST ? BP_MOTOR_COAST : BP_MOTOR_DUTY, 0, 0);
			}
			err = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_drivebase_settings_set(enum bp_port_owner owner, uint32_t handle,
	const struct bp_drivebase_settings *settings, uint32_t *request)
{
	if (!settings || !request) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct drivebase *db = find_drivebase(owner, handle);
	int err = atomic_get(&stopping) ? -ESHUTDOWN : !db || db->detach ? -ENODEV :
		db->value.pending ? -EBUSY : db->value.error;
	if (!err) {
		err = k_work_reschedule_for_queue(PORT_QUEUE, &work, K_NO_WAIT);
		if (err >= 0) {
			db->settings = *settings;
			db->configure = DRIVE_CONFIG_BASIC;
			db->value.command_error = 0;
			db->value.pending = true;
			*request = ++db->value.request;
			err = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_drivebase_close(enum bp_port_owner owner, uint32_t handle)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct drivebase *db = find_drivebase(owner, handle);
	int err = !db ? -ENODEV : k_work_reschedule_for_queue(PORT_QUEUE, &work, K_NO_WAIT);
	if (err >= 0) {
		cancel_drivebase(db->motor[0], -ECANCELED, true);
		err = 0;
	}
	k_spin_unlock(&lock, key);
	return err;
}

/* Axis settings are published by the same worker as pair state. Requests
 * share pair acknowledgment, but never replace the active motion token. */
int hub_drivebase_axis_get(enum bp_port_owner owner, uint32_t handle, uint32_t axis,
	struct bp_drivebase_axis_state *value)
{
	if (axis >= DRIVEBASE_AXES || !value) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct drivebase *db = find_drivebase(owner, handle);
	int err = !db || db->detach ? -ENODEV : db->value.error;
	if (!err) err = !db->value.valid ? -EAGAIN :
		k_uptime_get() - db->value.sample_ms >= 2 * CONTROL_MS ? -ETIMEDOUT : 0;
	if (!err) *value = db->axis[axis];
	k_spin_unlock(&lock, key);
	return err;
}

int hub_drivebase_axis_set(enum bp_port_owner owner, uint32_t handle, uint32_t axis, uint32_t group,
	const struct bp_servo_settings *value, uint32_t *request)
{
	if (axis >= DRIVEBASE_AXES || group > BP_SERVO_STALL_TOLERANCES || !value || !request) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct drivebase *db = find_drivebase(owner, handle);
	int err = atomic_get(&stopping) ? -ESHUTDOWN : !db || db->detach ? -ENODEV :
		db->value.pending ? -EBUSY : db->value.error;
	if (!err && (value->revision != db->axis[axis].settings.revision ||
	    value->scale != db->axis[axis].settings.scale)) err = -EAGAIN;
	if (!err) {
		err = k_work_reschedule_for_queue(PORT_QUEUE, &work, K_NO_WAIT);
		if (err >= 0) {
			db->axis_settings = *value;
			db->settings_axis = axis;
			db->settings_group = group;
			db->configure = DRIVE_CONFIG_AXIS;
			db->value.command_error = 0;
			db->value.pending = true;
			*request = ++db->value.request;
			err = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_drivebase_log_start(enum bp_port_owner owner, uint32_t handle, uint32_t axis,
	uint32_t duration_ms, uint32_t down_sample)
{
	if (axis >= DRIVEBASE_AXES) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct drivebase *db = find_drivebase(owner, handle);
	int err = !db || db->detach ? -ENODEV : db->value.error;
	if (!err) err = !db->value.valid ? -EAGAIN : start_motor_log(&db->log[axis], owner,
		handle, duration_ms, down_sample, BP_CONTROL_LOG_COLUMNS);
	k_spin_unlock(&lock, key);
	return err;
}

/* Retain stopped rows after disconnection until application cleanup or reuse
 * of this bounded pair slot. Old handles can never read a replacement's log. */
static struct motor_log *drivebase_log(enum bp_port_owner owner, uint32_t handle, uint32_t axis)
{
	for (size_t i = 0; i < ARRAY_SIZE(drivebases); i++) {
		struct drivebase *db = drivebases[i];
		if (db && db->handle == handle && (db->owner == owner || db->log[axis].owner == owner)) return &db->log[axis];
	}
	return NULL;
}

int hub_drivebase_log_status(enum bp_port_owner owner, uint32_t handle, uint32_t axis,
	uint32_t stop, struct bp_motor_log_status *value)
{
	if (axis >= DRIVEBASE_AXES || !value || stop > 1 ||
	    !owner_valid(owner)) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct motor_log *log = drivebase_log(owner, handle, axis);
	if (log) {
		if (stop) log->status.active = false;
		*value = log->status;
	}
	k_spin_unlock(&lock, key);
	return log ? 0 : -ENODEV;
}

int hub_drivebase_log_read(enum bp_port_owner owner, uint32_t handle, uint32_t axis,
	uint32_t revision, uint32_t row, struct bp_motor_log_row *value)
{
	if (axis >= DRIVEBASE_AXES || !value ||
	    !owner_valid(owner)) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct motor_log *log = drivebase_log(owner, handle, axis);
	int err = log ? read_motor_log(log, revision, row, value) : -ENODEV;
	k_spin_unlock(&lock, key);
	return err;
}

bool hub_drivebase_uses_gyro(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool used = false;
	for (size_t i = 0; i < ARRAY_SIZE(drivebases); i++) {
		const struct drivebase *db = drivebases[i];
		used |= db && db->owner && !db->detach && db->value.gyro && (db->value.active || db->value.pending);
	}
	k_spin_unlock(&lock, key);
	return used;
}
#else
bool hub_drivebase_uses_gyro(void) { return false; }
int hub_drivebase_open(enum bp_port_owner owner, const struct bp_drivebase_setup *setup, uint32_t *handle) { return -ENOTSUP; }
int hub_drivebase_get(enum bp_port_owner owner, uint32_t handle, struct bp_drivebase_state *state) { return -ENOTSUP; }
int hub_drivebase_command(enum bp_port_owner owner, uint32_t handle,
	const struct bp_drivebase_command *command, uint32_t *request) { return -ENOTSUP; }
int hub_drivebase_settings_set(enum bp_port_owner owner, uint32_t handle,
	const struct bp_drivebase_settings *settings, uint32_t *request) { return -ENOTSUP; }
int hub_drivebase_close(enum bp_port_owner owner, uint32_t handle) { return -ENOTSUP; }
int hub_drivebase_axis_get(enum bp_port_owner owner, uint32_t handle, uint32_t axis, struct bp_drivebase_axis_state *value) { return -ENOTSUP; }
int hub_drivebase_axis_set(enum bp_port_owner owner, uint32_t handle, uint32_t axis, uint32_t group, const struct bp_servo_settings *value, uint32_t *request) { return -ENOTSUP; }
int hub_drivebase_log_start(enum bp_port_owner owner, uint32_t handle, uint32_t axis, uint32_t duration_ms, uint32_t down_sample) { return -ENOTSUP; }
int hub_drivebase_log_status(enum bp_port_owner owner, uint32_t handle, uint32_t axis, uint32_t stop, struct bp_motor_log_status *value) { return -ENOTSUP; }
int hub_drivebase_log_read(enum bp_port_owner owner, uint32_t handle, uint32_t axis, uint32_t revision, uint32_t row, struct bp_motor_log_row *value) { return -ENOTSUP; }
#endif

int hub_servo_settings_get(uint32_t port, uint32_t generation, struct bp_servo_settings *value)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || !value) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = servo_control_error(p, generation);
	pbio_control_settings_t settings = p->control_settings;
	pbio_observer_settings_t model_settings = p->model_settings;
	*value = (struct bp_servo_settings){.revision = p->settings_revision, .applied = p->settings_applied};
	int32_t scale = p->motor.gear_ratio;
	k_spin_unlock(&lock, key);
	if (!err) {
		motor_control_settings_get(&settings, scale, value);
		motor_model_settings_get(&model_settings, &value->model);
	}
	return err;
#endif
}

int hub_servo_settings_set(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	uint32_t group, const struct bp_servo_settings *value, uint32_t *revision)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || !value || !revision ||
	    !owner_valid(owner)) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = servo_control_error(p, generation);
	if (!err && (p->owner != owner || p->motor.pending || p->motor.releasing)) err = -EBUSY;
	if (!err && (p->settings_revision != value->revision || p->motor.gear_ratio != value->scale)) err = -EAGAIN;
	pbio_control_settings_t settings = p->control_settings;
	pbio_observer_settings_t model_settings = p->model_settings;
	uint16_t type = p->value.type_id;
	uint32_t request = p->motor.request;
	k_spin_unlock(&lock, key);
	if (err) return err;
	err = group == BP_SERVO_MODEL ? motor_model_settings_set(type, &model_settings, &value->model) :
		motor_control_settings_set(&settings, group, value);
	if (err) return err;
	key = k_spin_lock(&lock);
	err = servo_control_error(p, generation);
	if (!err && (p->owner != owner || p->motor.request != request ||
	    p->settings_revision != value->revision)) err = -EAGAIN;
	if (!err) {
		p->control_settings = settings;
		p->model_settings = model_settings;
		*revision = ++p->settings_revision;
	}
	k_spin_unlock(&lock, key);
	return err;
#endif
}

int hub_servo_trajectory_get(uint32_t port, uint32_t generation, struct bp_servo_trajectory *value)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || !value) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = servo_control_error(p, generation);
	if (!err) *value = p->trajectory;
	int32_t direction = p->motor.direction;
	k_spin_unlock(&lock, key);
	/* Reference PBIO applies direction in its tacho before planning. Our
	 * controller uses physical shaft coordinates and converts at this API. */
	if (!err) {
		for (size_t i = 4; i < ARRAY_SIZE(value->points); i++) value->points[i] *= direction;
		value->load *= direction;
	}
	return err;
#endif
}

int hub_servo_model_get(uint32_t port, uint32_t generation, struct bp_motor_model_state *value)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || !value) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = servo_control_error(p, generation);
	if (!err) *value = p->model_state;
	int64_t zero = p->motor.zero_mdeg;
	int32_t direction = p->motor.direction;
	k_spin_unlock(&lock, key);
	if (err) return err;
	if (__builtin_sub_overflow(value->angle_mdeg, zero, &value->angle_mdeg) ||
	    __builtin_mul_overflow(value->angle_mdeg, direction, &value->angle_mdeg)) return -ERANGE;
	value->speed_mdeg_s *= direction;
	value->current_tenth_ma *= direction;
	return 0;
#endif
}

int hub_motor_log_start(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t kind,
	uint32_t duration_ms, uint32_t down_sample)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || kind >= BP_MOTOR_LOG_COUNT || !down_sample ||
	    !owner_valid(owner)) return -EINVAL;
	uint32_t columns = kind == BP_MOTOR_LOG ? BP_MOTOR_LOG_COLUMNS : BP_CONTROL_LOG_COLUMNS;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	struct motor_log *log = &p->log[kind];
	int err = servo_control_error(p, generation);
	if (!err && kind == BP_CONTROL_LOG && p->drivebase &&
	    (p->drivebase->value.active || p->drivebase->value.pending)) err = -EBUSY;
	if (!err && (p->motor.releasing || p->owner != owner ||
	    (log->owner && log->owner != owner))) err = -EBUSY;
	if (!err) err = start_motor_log(log, owner, generation, duration_ms, down_sample, columns);
	k_spin_unlock(&lock, key);
	return err;
#endif
}

#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
static int motor_log_access(struct port *p, struct motor_log *log, enum bp_port_owner owner, uint32_t generation)
{
	if (log->owner && log->owner != owner) return -EBUSY;
	/* A stopped log outlives Motor.close or disconnection, until application
	 * cleanup or a later start replaces it. Reading never energizes a port. */
	return log->owner ? log->generation == generation ? 0 : -ESTALE :
		p->value.generation == generation ? 0 : -ESTALE;
}
#endif

int hub_motor_log_status(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t kind,
	uint32_t stop, struct bp_motor_log_status *value)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || kind >= BP_MOTOR_LOG_COUNT || !value || stop > 1 ||
	    !owner_valid(owner)) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	struct motor_log *log = &p->log[kind];
	int err = motor_log_access(p, log, owner, generation);
	if (!err) {
		if (stop) log->status.active = false;
		*value = log->status;
	}
	k_spin_unlock(&lock, key);
	return err;
#endif
}

int hub_motor_log_read(enum bp_port_owner owner, uint32_t port, uint32_t generation, uint32_t kind,
	uint32_t revision, uint32_t row, struct bp_motor_log_row *value)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || kind >= BP_MOTOR_LOG_COUNT || !value ||
	    !owner_valid(owner)) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	struct motor_log *log = &p->log[kind];
	int err = motor_log_access(p, log, owner, generation);
	if (!err) err = read_motor_log(log, revision, row, value);
	k_spin_unlock(&lock, key);
	return err;
#endif
}

int hub_servo_move(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	const struct bp_servo_command *command, uint32_t *request)
{
#ifndef CONFIG_BRICKPHYR_MOTOR_CONTROL
	return -ENOTSUP;
#else
	if (port >= BP_PORT_COUNT || !command || !request ||
	    !owner_valid(owner) ||
	    command->action > BP_SERVO_UNTIL_STALLED_TORQUE || command->finish > BP_MOTOR_FINISH_BRAKE_SMART ||
	    command->duration_ms >= PBIO_TRAJECTORY_DURATION_FOREVER_MS ||
	    (command->action != BP_SERVO_TIME && command->duration_ms) ||
	    ((command->action == BP_SERVO_UNTIL_STALLED || command->action == BP_SERVO_UNTIL_STALLED_TORQUE) &&
	     (command->finish == BP_MOTOR_FINISH_CONTINUE || command->duty_limit < -1 || command->duty_limit > 1000))) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	struct bp_battery_sample battery;
	int err = hub_battery_get(&battery);
	if (!err) err = battery.error;
	if (!err && (!battery.voltage_mv || battery.voltage_mv > 12000)) err = -ERANGE;
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	struct bp_motor_state *m = &p->motor;
	err = motor_connection_error(p, owner, generation);
	if (!err) err = p->owner != owner || !m->gear_ratio || !m->control.observing ? -ENODEV :
		m->pending ? -EBUSY : m->error ? m->error : output_device_error(p, false);
	if (!err) err = control_input_error(p);
	int32_t limit = motor_control_speed_limit(p->value.type_id);
	int64_t speed = (int64_t)command->speed * m->gear_ratio * m->direction;
	int64_t target = (int64_t)command->angle * m->gear_ratio * m->direction;
	struct bp_motor_command raw = {.mode = BP_MOTOR_POSITION, .finish = command->finish,
		.speed_mdeg_s = CLAMP(speed, -limit, limit), .duration_ms = command->duration_ms,
		.duty_limit = command->duty_limit};
	switch (command->action) {
	case BP_SERVO_RUN:
	case BP_SERVO_TIME:
	case BP_SERVO_UNTIL_STALLED:
	case BP_SERVO_UNTIL_STALLED_TORQUE:
		raw.mode = BP_MOTOR_SPEED;
		raw.action = command->action == BP_SERVO_TIME ? BP_MOTOR_TIMED :
			command->action == BP_SERVO_UNTIL_STALLED_TORQUE ? BP_MOTOR_UNTIL_STALLED_TORQUE :
			command->action == BP_SERVO_UNTIL_STALLED ? BP_MOTOR_UNTIL_STALLED : BP_MOTOR_TRAJECTORY;
		if (command->action == BP_SERVO_RUN) raw.finish = BP_MOTOR_FINISH_CONTINUE;
		break;
	case BP_SERVO_ANGLE:
		raw.action = BP_MOTOR_RELATIVE;
		/* Resolve both signs once. PBIO sees positive speed and a signed
		 * physical displacement, retaining its smart relative endpoint. */
		raw.target_mdeg = !command->speed ? 0 : command->speed < 0 ? -target : target;
		raw.speed_mdeg_s = abs(raw.speed_mdeg_s);
		break;
	case BP_SERVO_TARGET:
		if (!command->speed) {
			raw.action = BP_MOTOR_RELATIVE;
			break;
		}
		raw.speed_mdeg_s = abs(raw.speed_mdeg_s);
		if (__builtin_add_overflow(m->zero_mdeg, target, &raw.target_mdeg)) err = -ERANGE;
		break;
	case BP_SERVO_TRACK:
		raw.action = BP_MOTOR_TRACK;
		raw.finish = BP_MOTOR_FINISH_HOLD;
		if (__builtin_add_overflow(m->zero_mdeg, target, &raw.target_mdeg)) err = -ERANGE;
		break;
	case BP_SERVO_HOLD:
		raw.action = BP_MOTOR_HOLD;
		raw.finish = BP_MOTOR_FINISH_HOLD;
		break;
	}
	if (!err) err = motor_control_check(p->value.type_id, uart_state(p)->sensor.angle_mdeg, &raw);
	if (!err) err = request_motor(p, raw.mode, 0, 0);
	if (!err) {
		m->command = raw;
		*request = m->request;
	}
	k_spin_unlock(&lock, key);
	return err;
#endif
}

int hub_motor_set_voltage(enum bp_port_owner owner, uint32_t port, uint32_t generation, int32_t max_voltage)
{
	if (port >= BP_PORT_COUNT || !owner_valid(owner)) return -EINVAL;
	if (!port_present(port)) return -ENODEV;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	int err = motor_connection_error(p, owner, generation);
	if (!err) err = !p->motor.direction || p->owner != owner ? -ENODEV :
		max_voltage < 0 || max_voltage > motor_max_voltage(p->value.type_id) ? -EINVAL : 0;
	if (!err) p->motor.max_voltage = max_voltage;
	k_spin_unlock(&lock, key);
	return err;
}

int hub_motor_dc(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	int32_t duty, uint32_t coast, uint32_t *request)
{
	if (port >= BP_PORT_COUNT || !request || duty < -1000 || duty > 1000 || coast > 1 || (coast && duty) ||
	    !owner_valid(owner)) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	/* Reading the battery snapshot never nests its lock inside the port lock.
	 * A missing battery sample must not prevent coast or active braking. */
	struct bp_battery_sample battery;
	int battery_error = duty ? hub_battery_get(&battery) : 0;
	if (duty && !battery_error) battery_error = battery.error ? battery.error :
		(!battery.voltage_mv || battery.voltage_mv > 12000) ? -ERANGE : 0;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	err = motor_connection_error(p, owner, generation);
	if (!err) err = !p->motor.direction || p->owner != owner ? -ENODEV :
		!coast && p->motor.pending ? -EBUSY : coast ? 0 : output_device_error(p, false);
	if (!err) err = battery_error;
	if (!err && duty) {
		/* PBIO dcmotor.c applies direction and the model's voltage cap after
		 * converting duty to battery-relative voltage. Native PWM uses permille. */
		int32_t voltage = duty * (int32_t)battery.voltage_mv / 1000;
		voltage = CLAMP(voltage, -p->motor.max_voltage, p->motor.max_voltage);
		duty = voltage * 1000 / (int32_t)battery.voltage_mv * p->motor.direction;
	}
	if (!err) err = request_motor(p, coast ? BP_MOTOR_COAST : BP_MOTOR_DUTY, duty, 0);
	if (!err) {
		p->motor.error = p->motor.cleanup_error = 0;
		*request = p->motor.request;
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_motor_drive(enum bp_port_owner owner, uint32_t port, int32_t duty, uint32_t duration_ms)
{
	if (port >= BP_PORT_COUNT || duty < -1000 || duty > 1000 || duration_ms > INT32_MAX ||
	    !owner_valid(owner)) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	err = atomic_get(&stopping) ? -ESHUTDOWN : p->motor.releasing || p->motor.pending || write_pending(p) ||
		(p->owner && p->owner != owner) ? -EBUSY : output_device_error(p, false);
	if (!err) err = request_motor(p, BP_MOTOR_DUTY, duty, duration_ms);
	if (!err) {
		p->owner = owner;
		p->motor.error = p->motor.cleanup_error = 0;
	}
	k_spin_unlock(&lock, key);
	return err;
}

int hub_port_light_write(enum bp_port_owner owner, uint32_t port, uint32_t generation,
	uint32_t brightness, uint32_t *request)
{
	if (port >= BP_PORT_COUNT || brightness > 1000 || !request ||
	    !owner_valid(owner)) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	err = atomic_get(&stopping) ? -ESHUTDOWN : p->value.generation != generation ? -ESTALE :
		p->motor.releasing || (brightness && p->motor.pending) || write_pending(p) ||
		(p->owner && p->owner != owner) ? -EBUSY : output_device_error(p, true);
	if (!err) err = request_motor(p, brightness ? BP_MOTOR_DUTY : BP_MOTOR_COAST, brightness, 0);
	if (!err) {
		p->owner = owner;
		p->motor.error = p->motor.cleanup_error = 0;
		*request = p->motor.request;
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
	    !owner_valid(owner)) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	struct bp_battery_sample battery;
	err = hub_battery_get(&battery);
	if (!err) err = battery.error;
	if (!err && (!battery.voltage_mv || battery.voltage_mv > 12000)) err = -ERANGE;
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	err = atomic_get(&stopping) ? -ESHUTDOWN : p->motor.releasing || p->motor.pending || write_pending(p) ||
		(p->owner && p->owner != owner) ? -EBUSY : output_device_error(p, false);
	if (!err) err = control_input_error(p);
	if (!err) err = motor_control_check(p->value.type_id, uart_state(p)->sensor.angle_mdeg, command);
	if (!err) err = request_control(p, command->mode);
	if (!err) {
		p->motor.command = *command;
		p->owner = owner;
		p->motor.error = p->motor.cleanup_error = 0;
		start_observation(p);
	}
	k_spin_unlock(&lock, key);
	return err;
#endif
}

int hub_motor_coast(enum bp_port_owner owner, uint32_t port)
{
	if (port >= BP_PORT_COUNT || !owner_valid(owner)) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct port *p = &ports[port];
	err = atomic_get(&stopping) ? -ESHUTDOWN :
		serial_mode(p) || (p->owner && p->owner != owner) ? -EBUSY :
		p->owner && (p->motor.pending || p->motor.mode != BP_MOTOR_COAST ||
			!p->motor.valid || p->motor.releasing) ? 0 : output_device_error(p, false);
	if (!err) err = request_motor(p, BP_MOTOR_COAST, 0, 0);
	if (!err) {
		p->owner = owner;
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
#ifdef CONFIG_BRICKPHYR_MOTORS
	k_spinlock_key_t group_key = k_spin_lock(&lock);
	if (motor_group.value.active || motor_group.value.pending) cancel_motor_group(-ESHUTDOWN);
	k_spin_unlock(&lock, group_key);
#endif
	for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
		port_uart_stop(i);
	}
	int supply_error = gpio_is_ready_dt(&supply) ? gpio_pin_set_dt(&supply, 0) : -ENODEV;
	int result = supply_error;
	for (uint32_t i = 0; i < ARRAY_SIZE(ports); i++) {
		if (!pins[i][P5].port) {
			continue;
		}
		invalidate_motor(i, -ESHUTDOWN);
		int err = release_pins(i);
		k_spinlock_key_t serial_key = k_spin_lock(&lock);
		if (!err && IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) uart_state(&ports[i])->serial = SERIAL_OFF;
		k_spin_unlock(&lock, serial_key);
		finish_reset(i, err);
		if (!result) {
			result = err;
		}
		k_spinlock_key_t key = k_spin_lock(&lock);
#ifdef CONFIG_BRICKPHYR_MOTOR_CONTROL
		/* Shutdown joined the worker above, including outside-lock frames. */
		k_free(ports[i].controller);
		ports[i].controller = NULL;
#endif
		finish_resync(&ports[i], -ESHUTDOWN, 0);
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
		if (IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) {
			uart_state(&ports[i])->sensor.fields = 0;
			uart_state(&ports[i])->sensor.available = 0;
			uart_state(&ports[i])->sensor.type_id = 0;
			uart_state(&ports[i])->sensor.generation = v->generation;
			if (!uart_state(&ports[i])->sensor.error || uart_state(&ports[i])->sensor.error == -EAGAIN) {
				uart_state(&ports[i])->sensor.error = cleanup ? cleanup : -ESHUTDOWN;
			}
		}
		if (!v->error || v->error == -EAGAIN) {
			v->error = cleanup ? cleanup : -ESHUTDOWN;
		}
		v->cleanup_error = cleanup;
		k_spin_unlock(&lock, key);
	}
#ifdef CONFIG_BRICKPHYR_MOTORS
	group_key = k_spin_lock(&lock);
	complete_motor_group();
	k_spin_unlock(&lock, group_key);
#endif
	return result;
}
