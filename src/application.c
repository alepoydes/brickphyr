// SPDX-License-Identifier: Apache-2.0
#include "application.h"
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
#include "app_store.h"
#include <zephyr/llext/fs_loader.h>
#endif
#include "native_image.h"
#include "application_private.h"
#ifdef CONFIG_BRICKPHYR_PYTHON
#include "python_runtime.h"
#include <zephyr/sys/libc-hooks.h>
#endif
#include "hub.h"
#include "hub_matrix.h"
#include "hub_light.h"
#include "hub_sound.h"
#include "hub_imu.h"
#include "hub_battery.h"
#include "hub_ports.h"
#include "hub_power.h"

#include <brickphyr.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/app_memory/mem_domain.h>
#include <zephyr/fatal.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/kernel.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/llext/llext.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/barrier.h>

/* A timer wakeup can reset equal-priority time slicing. Control and diagnostics
 * must preempt the application, even when its entry never yields. */
BUILD_ASSERT(CONFIG_SHELL_THREAD_PRIORITY_OVERRIDE &&
	     CONFIG_SHELL_THREAD_PRIORITY < K_LOWEST_APPLICATION_THREAD_PRIO);
BUILD_ASSERT(CONFIG_LOG_PROCESS_THREAD_CUSTOM_PRIORITY &&
	     CONFIG_LOG_PROCESS_THREAD_PRIORITY < K_LOWEST_APPLICATION_THREAD_PRIO);

K_MUTEX_DEFINE(lifecycle);
/* A single application stack: no thread creation API or inherited objects. */
K_THREAD_STACK_DEFINE(application_stack, CONFIG_BRICKPHYR_APPLICATION_STACK_SIZE);
static struct k_thread application_thread;
static struct k_mem_domain application_domain;
static bool domain_initialized;
static struct llext *extension;
static bool loader_initialized;
/* One execution arena for all engines. Linker-managed MPU alignment avoids
 * doubling the working allocation merely to obtain its aligned address. The
 * system heap holds transfers and firmware allocations, not a second VM heap. */
K_APPMEM_PARTITION_DEFINE(execution_partition);
static K_APP_BMEM(execution_partition) uint8_t execution_memory[APPLICATION_MEMORY_LIMIT];
static struct application_status status;
static atomic_t return_value;
static atomic_t fault_reason;
static atomic_t active;
#define OUTPUT_CAPACITY 1024
RING_BUF_DECLARE(output, OUTPUT_CAPACITY);
static struct k_spinlock output_lock;
static uint32_t output_dropped;
static bool output_attached;
static void *upload;
static size_t upload_size;
static size_t upload_bytes;
static size_t upload_heap;
static enum application_format upload_format;
static atomic_t session_generation;
static atomic_val_t upload_generation;
static bool shutting_down;

/* Native logging never waits. Python may leave an unwritten suffix with the VM
 * while a continuous consumer is attached; both use this one bounded queue. */
static int write_output(const void *data, size_t length, bool stream)
{
	if (length > 128) {
		return -EMSGSIZE;
	}
	k_spinlock_key_t key = k_spin_lock(&output_lock);
	bool attached = stream && output_attached;
	size_t count;
	if (!length && stream) {
		count = !attached || ring_buf_space_get(&output);
	} else {
		count = ring_buf_put(&output, data, length);
		if (!attached) {
			output_dropped = MIN((uint64_t)output_dropped + length - count, UINT32_MAX);
			if (stream) {
				/* Detached Python writes consume their input into the log, even
				 * when it overflows. They must not replay it after reconnect. */
				count = length;
			}
		}
	}
	k_spin_unlock(&output_lock, key);
	return count;
}

#ifdef CONFIG_BRICKPHYR_PYTHON
/* Two maximum-size input fragments. Backpressure retains excess on the host;
 * increasing this queue cannot compensate for a program that does not read. */
RING_BUF_DECLARE(input, 256);
static struct k_spinlock input_lock;
K_SEM_DEFINE(io_ready, 0, 1);
static atomic_val_t input_generation;

/* input_lock is held. A detached REPL loses its unfinished command line, not
 * its Python state or running command. Accepted script input remains owned by
 * the script, as before. Refresh on both producer and consumer before new I/O. */
static void refresh_input(void)
{
	atomic_val_t generation = atomic_get(&session_generation);
	if (input_generation != generation) {
		input_generation = generation;
		if (status.repl) {
			ring_buf_reset(&input);
			if (python_repl_reading()) {
				uint8_t cancel = 3;
				ring_buf_put(&input, &cancel, 1);
			}
		}
	}
}

static int run_python(const void *program, size_t length, enum application_format format,
	bool soft_reset);

/* Allocated only when a program first compiles/loads native code. The allocator
 * needs up to twice this size transiently for MPU alignment. */
#define PYTHON_CODE_CAPACITY (16 * 1024)
BUILD_ASSERT(!IS_ENABLED(CONFIG_DCACHE) && !IS_ENABLED(CONFIG_ICACHE));
static void *code_memory;
static size_t code_used, code_reserved;
static struct {
	bool reserve;
	const void *data;
	size_t length;
	intptr_t result;
} code_request;
static atomic_t code_pending;
K_SEM_DEFINE(code_done, 0, 1);

static intptr_t reserve_code(size_t length)
{
	if (!length || length > PYTHON_CODE_CAPACITY - code_used) {
		return -ENOMEM;
	}
	if (code_reserved) {
		return -EBUSY;
	}
	if (!code_memory) {
		code_memory = k_aligned_alloc(PYTHON_CODE_CAPACITY, PYTHON_CODE_CAPACITY);
		if (!code_memory) {
			return -ENOMEM;
		}
		/* Do not expose previous system-heap contents to the application. */
		memset(code_memory, 0, PYTHON_CODE_CAPACITY);
		struct k_mem_partition code = {
			.start = (uintptr_t)code_memory, .size = PYTHON_CODE_CAPACITY,
			.attr = K_MEM_PARTITION_P_RX_U_RX,
		};
		int err = k_mem_domain_add_partition(&application_domain, &code);
		if (err) {
			k_free(code_memory);
			code_memory = NULL;
			return err;
		}
		status.code_bytes = PYTHON_CODE_CAPACITY;
	}
	code_reserved = length;
	return (intptr_t)((uint8_t *)code_memory + code_used);
}

static void code_work_run(struct k_work *work)
{
	ARG_UNUSED(work);
	/* The application is blocked. This privileged worker has its own MPU map:
	 * it can fill new code while the application's region stays read/execute.
	 * Serializing with stop prevents copying from an unloaded application. */
	k_mutex_lock(&lifecycle, K_FOREVER);
	if (atomic_get(&code_pending)) {
		intptr_t result = -EINVAL;
		if (code_request.reserve) {
			result = reserve_code(code_request.length);
		} else if (!code_request.data && !code_request.length) {
			code_reserved = 0;
			result = 0;
		} else if (code_reserved && code_request.length == code_reserved) {
			memcpy((uint8_t *)code_memory + code_used, code_request.data, code_reserved);
			barrier_dsync_fence_full();
			barrier_isync_fence_full();
			code_used += ROUND_UP(code_reserved, 4);
			status.code_used = code_used;
			code_reserved = 0;
			result = 0;
		}
		code_request.result = result;
		atomic_clear(&code_pending);
		k_sem_give(&code_done);
	}
	k_mutex_unlock(&lifecycle);
}
static K_WORK_DEFINE(code_work, code_work_run);

static intptr_t code_exchange(bool reserve, const void *data, size_t length)
{
	code_request.reserve = reserve;
	code_request.data = data;
	code_request.length = length;
	atomic_set(&code_pending, 1);
	int err = k_work_submit(&code_work);
	if (err < 0) {
		atomic_clear(&code_pending);
		return err;
	}
	k_sem_take(&code_done, K_FOREVER);
	return code_request.result;
}

static void verify_python_thread(void)
{
	K_OOPS(K_SYSCALL_VERIFY(atomic_get(&active) &&
		k_sched_current_thread_query() == &application_thread &&
		status.format != APPLICATION_NATIVE));
}

int z_impl_application_stdin(void *data, size_t capacity)
{
	k_spinlock_key_t key = k_spin_lock(&input_lock);
	refresh_input();
	int count = capacity ? ring_buf_get(&input, data, capacity) : ring_buf_size_get(&input);
	k_spin_unlock(&input_lock, key);
	return count;
}
static inline int z_vrfy_application_stdin(void *data, size_t capacity)
{
	verify_python_thread();
	if (capacity > 128) {
		return -EMSGSIZE;
	}
	if (capacity) {
		K_OOPS(K_SYSCALL_MEMORY_WRITE(data, capacity));
	}
	return z_impl_application_stdin(data, capacity);
}
#include <zephyr/syscalls/application_stdin_mrsh.c>

int z_impl_application_stdout(const void *data, size_t length)
{
	return write_output(data, length, true);
}
static inline int z_vrfy_application_stdout(const void *data, size_t length)
{
	verify_python_thread();
	if (length > 128) {
		return -EMSGSIZE;
	}
	if (length) {
		K_OOPS(K_SYSCALL_MEMORY_READ(data, length));
	}
	return z_impl_application_stdout(data, length);
}
#include <zephyr/syscalls/application_stdout_mrsh.c>

void application_output_stream(bool attached)
{
	k_spinlock_key_t key = k_spin_lock(&output_lock);
	bool changed = output_attached != attached;
	output_attached = attached;
	k_spin_unlock(&output_lock, key);
	if (changed) {
		k_sem_give(&io_ready);
	}
}

int z_impl_application_event_wait(int32_t timeout_ms)
{
	if (timeout_ms < -1) {
		return -EINVAL;
	}
	return k_sem_take(&io_ready, timeout_ms == -1 ? K_FOREVER : K_MSEC(timeout_ms));
}
static inline int z_vrfy_application_event_wait(int32_t timeout_ms)
{
	verify_python_thread();
	return z_impl_application_event_wait(timeout_ms);
}
#include <zephyr/syscalls/application_event_wait_mrsh.c>

intptr_t z_impl_application_code_reserve(size_t length)
{
	return code_exchange(true, NULL, length);
}
static inline intptr_t z_vrfy_application_code_reserve(size_t length)
{
	verify_python_thread();
	return z_impl_application_code_reserve(length);
}
#include <zephyr/syscalls/application_code_reserve_mrsh.c>

int z_impl_application_code_commit(const void *data, size_t length)
{
	return code_exchange(false, data, length);
}
static inline int z_vrfy_application_code_commit(const void *data, size_t length)
{
	verify_python_thread();
	if (length > PYTHON_CODE_CAPACITY) {
		return -EINVAL;
	}
	if (length) {
		K_OOPS(K_SYSCALL_MEMORY_READ(data, length));
	}
	return z_impl_application_code_commit(data, length);
}
#include <zephyr/syscalls/application_code_commit_mrsh.c>
#endif

/* lifecycle is held for all staging access. A complete validated upload can
 * be started from a later session; an incomplete one cannot be resumed there. */
static void discard_upload(void)
{
	k_free(upload);
	upload = NULL;
	upload_size = upload_bytes = upload_heap = 0;
}

static void expire_upload(void)
{
	if (!upload_heap && upload_generation != atomic_get(&session_generation)) {
		discard_upload();
	}
}

void application_session_closed(void)
{
	atomic_inc(&session_generation);
#ifdef CONFIG_BRICKPHYR_PYTHON
	application_output_stream(false);
	k_sem_give(&io_ready);
#endif
}

void application_discard_upload(void)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	discard_upload();
	k_mutex_unlock(&lifecycle);
}

int64_t z_impl_bp_time_ms(void)
{
	return k_uptime_get();
}

static inline int64_t z_vrfy_bp_time_ms(void)
{
	return z_impl_bp_time_ms();
}
#include <zephyr/syscalls/bp_time_ms_mrsh.c>

int z_impl_bp_sleep(uint32_t milliseconds)
{
	if (milliseconds > INT32_MAX) {
		return -EINVAL;
	}
	if (milliseconds) {
		k_msleep(milliseconds);
	} else {
		k_yield();
	}
	return 0;
}

static inline int z_vrfy_bp_sleep(uint32_t milliseconds)
{
	return z_impl_bp_sleep(milliseconds);
}
#include <zephyr/syscalls/bp_sleep_mrsh.c>

int z_impl_bp_write(const void *data, size_t length)
{
	return write_output(data, length, false);
}

static inline int z_vrfy_bp_write(const void *data, size_t length)
{
	if (length > 128) {
		return -EMSGSIZE;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(data, length));
	return z_impl_bp_write(data, length);
}
#include <zephyr/syscalls/bp_write_mrsh.c>

uint32_t z_impl_bp_buttons(void)
{
	return hub_buttons();
}

static inline uint32_t z_vrfy_bp_buttons(void)
{
	return z_impl_bp_buttons();
}
#include <zephyr/syscalls/bp_buttons_mrsh.c>

int z_impl_bp_light_write(uint32_t light, const uint8_t *rgb)
{
	return hub_light_write(light, rgb);
}

static inline int z_vrfy_bp_light_write(uint32_t light, const uint8_t *rgb)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(rgb, 3));
	uint8_t copy[3];
	memcpy(copy, rgb, sizeof(copy));
	return z_impl_bp_light_write(light, copy);
}
#include <zephyr/syscalls/bp_light_write_mrsh.c>

int z_impl_bp_light_animate(uint32_t light, const struct bp_light_frame *frames, size_t count)
{
	return hub_light_animate(light, frames, count);
}

static inline int z_vrfy_bp_light_animate(uint32_t light, const struct bp_light_frame *frames,
	size_t count)
{
	if (!count || count > BP_LIGHT_MAX_FRAMES) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(frames, __alignof__(*frames))));
	K_OOPS(K_SYSCALL_MEMORY_READ(frames, count * sizeof(*frames)));
	return z_impl_bp_light_animate(light, frames, count);
}
#include <zephyr/syscalls/bp_light_animate_mrsh.c>

int z_impl_bp_light_get(uint32_t light, struct bp_light_state *state)
{
	return hub_light_get(light, state);
}

static inline int z_vrfy_bp_light_get(uint32_t light, struct bp_light_state *state)
{
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(state, __alignof__(*state))));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(state, sizeof(*state)));
	return z_impl_bp_light_get(light, state);
}
#include <zephyr/syscalls/bp_light_get_mrsh.c>

int z_impl_bp_matrix_write(uint32_t first, const uint8_t *values, size_t count)
{
	return hub_matrix_write(BP_MATRIX_APPLICATION, first, values, count);
}

static inline int z_vrfy_bp_matrix_write(uint32_t first, const uint8_t *values, size_t count)
{
	if (!count || first >= BP_MATRIX_PIXELS || count > BP_MATRIX_PIXELS - first) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(values, count));
	uint8_t copy[BP_MATRIX_PIXELS];
	memcpy(copy, values, count);
	return z_impl_bp_matrix_write(first, copy, count);
}
#include <zephyr/syscalls/bp_matrix_write_mrsh.c>

int z_impl_bp_matrix_animate(const uint8_t *frames, size_t count, uint32_t interval_ms)
{
	return hub_matrix_animate(frames, count, interval_ms);
}

static inline int z_vrfy_bp_matrix_animate(const uint8_t *frames, size_t count, uint32_t interval_ms)
{
	if (!count || count > BP_MATRIX_MAX_FRAMES) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(frames, count * BP_MATRIX_PIXELS));
	return z_impl_bp_matrix_animate(frames, count, interval_ms);
}
#include <zephyr/syscalls/bp_matrix_animate_mrsh.c>

int z_impl_bp_matrix_orientation(uint32_t rotation)
{
	return hub_matrix_orientation(rotation);
}

static inline int z_vrfy_bp_matrix_orientation(uint32_t rotation)
{
	return z_impl_bp_matrix_orientation(rotation);
}
#include <zephyr/syscalls/bp_matrix_orientation_mrsh.c>

int z_impl_bp_matrix_get(struct bp_matrix_state *value)
{
	return hub_matrix_get(value);
}

static inline int z_vrfy_bp_matrix_get(struct bp_matrix_state *value)
{
	/* Range validation alone permits misaligned memory. A resulting ARM
	 * fault inside the snapshot copy would strand the service's spinlock. */
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(value, __alignof__(*value))));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(value, sizeof(*value)));
	return z_impl_bp_matrix_get(value);
}
#include <zephyr/syscalls/bp_matrix_get_mrsh.c>

int z_impl_bp_imu_get(struct bp_imu_sample *value)
{
	return hub_imu_get(value);
}

static inline int z_vrfy_bp_imu_get(struct bp_imu_sample *value)
{
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(value, __alignof__(*value))));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(value, sizeof(*value)));
	return z_impl_bp_imu_get(value);
}
#include <zephyr/syscalls/bp_imu_get_mrsh.c>

int z_impl_bp_imu_get_state(struct bp_imu_state *value)
{
	return hub_imu_get_state(value);
}

static inline int z_vrfy_bp_imu_get_state(struct bp_imu_state *value)
{
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(value, __alignof__(*value))));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(value, sizeof(*value)));
	return z_impl_bp_imu_get_state(value);
}
#include <zephyr/syscalls/bp_imu_get_state_mrsh.c>

int z_impl_bp_battery_get(struct bp_battery_sample *value)
{
	return hub_battery_get(value);
}
static inline int z_vrfy_bp_battery_get(struct bp_battery_sample *value)
{
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(value, __alignof__(*value))));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(value, sizeof(*value)));
	return z_impl_bp_battery_get(value);
}
#include <zephyr/syscalls/bp_battery_get_mrsh.c>

int z_impl_bp_port_get(uint32_t port, struct bp_port_state *value)
{
	return hub_port_get(port, value);
}
static inline int z_vrfy_bp_port_get(uint32_t port, struct bp_port_state *value)
{
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(value, __alignof__(*value))));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(value, sizeof(*value)));
	return z_impl_bp_port_get(port, value);
}
#include <zephyr/syscalls/bp_port_get_mrsh.c>

int z_impl_bp_port_get_mode(uint32_t port, uint32_t generation, uint32_t mode,
	struct bp_port_mode *value)
{
	return hub_port_get_mode(port, generation, mode, value);
}
static inline int z_vrfy_bp_port_get_mode(uint32_t port, uint32_t generation, uint32_t mode,
	struct bp_port_mode *value)
{
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(value, __alignof__(*value))));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(value, sizeof(*value)));
	return z_impl_bp_port_get_mode(port, generation, mode, value);
}
#include <zephyr/syscalls/bp_port_get_mode_mrsh.c>

int z_impl_bp_sensor_get(uint32_t port, struct bp_sensor_sample *value)
{
	return hub_sensor_get(port, value);
}
static inline int z_vrfy_bp_sensor_get(uint32_t port, struct bp_sensor_sample *value)
{
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(value, __alignof__(*value))));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(value, sizeof(*value)));
	return z_impl_bp_sensor_get(port, value);
}
#include <zephyr/syscalls/bp_sensor_get_mrsh.c>

int z_impl_bp_sensor_configure(uint32_t port, uint32_t measurement)
{
	return hub_sensor_configure(BP_PORT_APPLICATION, port, measurement);
}
static inline int z_vrfy_bp_sensor_configure(uint32_t port, uint32_t measurement)
{
	return z_impl_bp_sensor_configure(port, measurement);
}
#include <zephyr/syscalls/bp_sensor_configure_mrsh.c>

int z_impl_bp_sensor_release(uint32_t port)
{
	return hub_port_release(BP_PORT_APPLICATION, port);
}
static inline int z_vrfy_bp_sensor_release(uint32_t port)
{
	return z_impl_bp_sensor_release(port);
}
#include <zephyr/syscalls/bp_sensor_release_mrsh.c>

int z_impl_bp_port_release(uint32_t port) { return hub_port_release(BP_PORT_APPLICATION, port); }
static inline int z_vrfy_bp_port_release(uint32_t port) { return z_impl_bp_port_release(port); }
#include <zephyr/syscalls/bp_port_release_mrsh.c>

int z_impl_bp_motor_drive(uint32_t port, int32_t duty, uint32_t duration_ms)
{
	return hub_motor_drive(BP_PORT_APPLICATION, port, duty, duration_ms);
}
static inline int z_vrfy_bp_motor_drive(uint32_t port, int32_t duty, uint32_t duration_ms)
{
	return z_impl_bp_motor_drive(port, duty, duration_ms);
}
#include <zephyr/syscalls/bp_motor_drive_mrsh.c>

int z_impl_bp_motor_coast(uint32_t port) { return hub_motor_coast(BP_PORT_APPLICATION, port); }
static inline int z_vrfy_bp_motor_coast(uint32_t port) { return z_impl_bp_motor_coast(port); }
#include <zephyr/syscalls/bp_motor_coast_mrsh.c>

int z_impl_bp_motor_get(uint32_t port, struct bp_motor_state *value) { return hub_motor_get(port, value); }
static inline int z_vrfy_bp_motor_get(uint32_t port, struct bp_motor_state *value)
{
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(value, __alignof__(*value))));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(value, sizeof(*value)));
	return z_impl_bp_motor_get(port, value);
}
#include <zephyr/syscalls/bp_motor_get_mrsh.c>

int z_impl_bp_motor_control(uint32_t port, const struct bp_motor_command *command)
{
	return hub_motor_control(BP_PORT_APPLICATION, port, command);
}
static inline int z_vrfy_bp_motor_control(uint32_t port, const struct bp_motor_command *command)
{
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(command, __alignof__(*command))));
	K_OOPS(K_SYSCALL_MEMORY_READ(command, sizeof(*command)));
	/* Copy user storage before taking any hardware service lock. */
	struct bp_motor_command copy = *command;
	return z_impl_bp_motor_control(port, &copy);
}
#include <zephyr/syscalls/bp_motor_control_mrsh.c>

int z_impl_bp_sound_tone(uint32_t frequency, uint32_t duration_ms, uint32_t volume)
{
	return hub_sound_tone(BP_SOUND_APPLICATION, frequency, duration_ms, volume);
}
static inline int z_vrfy_bp_sound_tone(uint32_t frequency, uint32_t duration_ms, uint32_t volume)
{
	return z_impl_bp_sound_tone(frequency, duration_ms, volume);
}
#include <zephyr/syscalls/bp_sound_tone_mrsh.c>

int z_impl_bp_sound_stream(uint32_t rate)
{
	return hub_sound_stream(BP_SOUND_APPLICATION, rate);
}
static inline int z_vrfy_bp_sound_stream(uint32_t rate)
{
	return z_impl_bp_sound_stream(rate);
}
#include <zephyr/syscalls/bp_sound_stream_mrsh.c>

int z_impl_bp_sound_write(const int16_t *samples, size_t count)
{
	return hub_sound_write(BP_SOUND_APPLICATION, samples, count);
}
static inline int z_vrfy_bp_sound_write(const int16_t *samples, size_t count)
{
	if (!count || count > BP_SOUND_CHUNK_SAMPLES) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(samples, __alignof__(*samples))));
	K_OOPS(K_SYSCALL_MEMORY_READ(samples, count * sizeof(*samples)));
	/* The bounded copy/conversion finishes under the service spinlock.
	 * No application pointer survives this syscall. */
	return z_impl_bp_sound_write(samples, count);
}
#include <zephyr/syscalls/bp_sound_write_mrsh.c>

int z_impl_bp_sound_finish(void)
{
	return hub_sound_finish(BP_SOUND_APPLICATION);
}
static inline int z_vrfy_bp_sound_finish(void) { return z_impl_bp_sound_finish(); }
#include <zephyr/syscalls/bp_sound_finish_mrsh.c>

int z_impl_bp_sound_stop(void)
{
	return hub_sound_stop(BP_SOUND_APPLICATION);
}
static inline int z_vrfy_bp_sound_stop(void) { return z_impl_bp_sound_stop(); }
#include <zephyr/syscalls/bp_sound_stop_mrsh.c>

int z_impl_bp_sound_get(struct bp_sound_state *value)
{
	return hub_sound_get(value);
}
static inline int z_vrfy_bp_sound_get(struct bp_sound_state *value)
{
	K_OOPS(K_SYSCALL_VERIFY(IS_ALIGNED(value, __alignof__(*value))));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(value, sizeof(*value)));
	return z_impl_bp_sound_get(value);
}
#include <zephyr/syscalls/bp_sound_get_mrsh.c>

/* Completion is a private syscall used by the user-mode trampoline. */
void z_impl_application_finished(int result)
{
	atomic_set(&return_value, result);
}

static inline void z_vrfy_application_finished(int result)
{
	K_OOPS(K_SYSCALL_VERIFY(k_sched_current_thread_query() == &application_thread));
	z_impl_application_finished(result);
}
#include <zephyr/syscalls/application_finished_mrsh.c>

static void native_entry(void *ext, void *function, void *unused)
{
	int result = llext_bringup(ext);
	if (!result) {
		int (*run)(void) = function;
		result = run();
		int cleanup = llext_teardown(ext);
		if (!result) {
			result = cleanup;
		}
	}
	application_finished(result);
}

#ifdef CONFIG_BRICKPHYR_PYTHON
static void python_entry(void *program, void *size, void *format)
{
	size_t length = (uintptr_t)size;
	size_t heap_size = sizeof(execution_memory) - ROUND_UP(length, 16);
	int result = python_execute(program, length,
		(uintptr_t)format, execution_memory, heap_size,
		(uintptr_t)K_THREAD_STACK_BUFFER(application_stack));
	application_finished(result);
}
#endif

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	/* A fault may precede initialization of the thread's TLS cache. */
	if (atomic_get(&active) && k_sched_current_thread_query() == &application_thread &&
	    reason != K_ERR_KERNEL_PANIC && esf && !(esf->basic.xpsr & 0x1ffU)) {
		atomic_set(&fault_reason, reason + 1);
		return; /* Zephyr aborts this thread; the supervisor joins before unload. */
	}
	k_fatal_halt(reason);
}

static int release(void)
{
	int err = hub_ports_release(BP_PORT_APPLICATION);
	/* A domain is initialized once. Remove public partition descriptors only
	 * after joining the thread, before freeing the memory they describe. */
	int sound_error = hub_sound_release(BP_SOUND_APPLICATION);
	if (!err) err = sound_error;
	int matrix_error = hub_matrix_release(BP_MATRIX_APPLICATION);
	if (!err) {
		err = matrix_error;
	}
	int light_error = hub_light_release();
	if (!err) err = light_error;
	if (!err && domain_initialized) {
		for (size_t i = 0; i < ARRAY_SIZE(application_domain.partitions); i++) {
			struct k_mem_partition part = application_domain.partitions[i];
			if (part.size && (err = k_mem_domain_remove_partition(&application_domain, &part))) {
				break;
			}
		}
	}
	if (!err && extension) {
		err = llext_unload(&extension);
	}
	if (!err && loader_initialized) {
		err = llext_heap_uninit();
		if (!err) {
			loader_initialized = false;
		}
	}
#ifdef CONFIG_BRICKPHYR_PYTHON
	/* A queued worker acquires lifecycle before reading the request. Joining
	 * its caller and clearing pending here also cancels an unstarted copy. */
	atomic_clear(&code_pending);
	k_sem_reset(&code_done);
	if (!err) {
		k_free(code_memory);
		code_memory = NULL;
		code_used = code_reserved = 0;
	}
#endif
	if (err && !status.cleanup_error) {
		status.cleanup_error = err;
	}
	if (!err) {
		hub_power_application(false);
	}
	return err;
}

/* lifecycle is held; a joined thread no longer holds its memory domain. */
static int reap(bool stop)
{
	if (!atomic_get(&active)) {
		return release();
	}
	bool ended = k_thread_join(&application_thread, K_NO_WAIT) == 0;
	if (!ended && !stop) {
		return -EAGAIN;
	}
	if (!ended) {
		k_thread_abort(&application_thread);
		int err = k_thread_join(&application_thread, K_FOREVER);
		if (err) {
			return err;
		}
	}
	atomic_clear(&active);
	int result = atomic_get(&return_value);
	status.result = result;
#ifdef CONFIG_BRICKPHYR_PYTHON
	if (status.repl && result == PYTHON_RESTART) {
		status.result = 0;
	}
#endif
	int fault = atomic_get(&fault_reason);
	status.fault = fault ? fault - 1 : 0;
	status.state = fault ? APPLICATION_FAULT : ended ? APPLICATION_RETURNED : APPLICATION_STOPPED;
	(void)k_thread_stack_space_get(&application_thread, &status.stack_unused);
	int err = release();
#ifdef CONFIG_BRICKPHYR_PYTHON
	/* A soft reset is a complete application lifetime boundary. Recursive
	 * prepare/reap(true) sees an already joined thread and cannot restart it. */
	if (!err && ended && !stop && !fault && status.repl &&
	    result == PYTHON_RESTART && !shutting_down) {
		return run_python(NULL, 0, APPLICATION_PYTHON, true);
	}
#endif
	return err;
}

int application_stop(void)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	int err = reap(true);
	k_mutex_unlock(&lifecycle);
	return err;
}

#ifdef CONFIG_BRICKPHYR_PYTHON
int application_input(const void *data, size_t length)
{
	if (length > 128 || (length && !data)) {
		return -EINVAL;
	}
	k_mutex_lock(&lifecycle, K_FOREVER);
	(void)reap(false);
	int err = -ESRCH;
	if (atomic_get(&active) && status.format != APPLICATION_NATIVE) {
		int interrupt_char = python_interrupt_char();
		size_t skip = 0;
		for (size_t i = 0; i < length; i++) {
			if (((const uint8_t *)data)[i] == interrupt_char) {
				skip = i + 1;
			}
		}
		k_spinlock_key_t key = k_spin_lock(&input_lock);
		refresh_input();
		if (skip) {
			ring_buf_reset(&input);
			data = (const uint8_t *)data + skip;
			length -= skip;
			python_interrupt();
		}
		err = length <= ring_buf_space_get(&input) ? 0 : -EAGAIN;
		if (!err) {
			ring_buf_put(&input, data, length);
		}
		k_spin_unlock(&input_lock, key);
		if (!err) {
			k_sem_give(&io_ready);
		}
	}
	k_mutex_unlock(&lifecycle);
	return err;
}

int application_interrupt(void)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	(void)reap(false);
	int err = -ESRCH;
	if (atomic_get(&active) && status.format != APPLICATION_NATIVE) {
		k_spinlock_key_t key = k_spin_lock(&input_lock);
		refresh_input();
		ring_buf_reset(&input);
		if (status.repl && python_repl_reading()) {
			uint8_t cancel = 3;
			ring_buf_put(&input, &cancel, 1);
		} else {
			python_interrupt();
		}
		k_spin_unlock(&input_lock, key);
		k_sem_give(&io_ready);
		err = 0;
	}
	k_mutex_unlock(&lifecycle);
	return err;
}
#endif

int application_shutdown(void)
{
	hub_power_request_shutdown();
	k_mutex_lock(&lifecycle, K_FOREVER);
	shutting_down = true;
	int err = reap(true);
	discard_upload();
	k_mutex_unlock(&lifecycle);
	return err;
}

void application_poll(void)
{
	if (k_mutex_lock(&lifecycle, K_NO_WAIT)) {
		return;
	}
	(void)reap(false);
	expire_upload();
	k_mutex_unlock(&lifecycle);
}

/* lifecycle is held; neither engine may replace a thread before it is joined. */
static int prepare(void)
{
	int err = reap(true);
	if (err) {
		return err;
	}
	if (!hub_power_application(true)) {
		return -ESHUTDOWN;
	}
	if (!domain_initialized) {
		err = k_mem_domain_init(&application_domain, 0, NULL);
		domain_initialized = err == 0;
	}
	if (err) {
		hub_power_application(false);
	}
	return err;
}

static int start_thread(k_thread_entry_t entry, void *first, void *second, void *third,
	struct application_status value, bool soft_reset)
{
	status = value;
	status.state = APPLICATION_RUNNING;
	status.stack_bytes = sizeof(application_stack);
	atomic_clear(&return_value);
	atomic_clear(&fault_reason);
#ifdef CONFIG_BRICKPHYR_PYTHON
	k_spinlock_key_t input_key = k_spin_lock(&input_lock);
	ring_buf_reset(&input);
	input_generation = atomic_get(&session_generation);
	k_spin_unlock(&input_lock, input_key);
	k_sem_reset(&io_ready);
	python_input_reset(soft_reset, value.repl);
#endif
	k_spinlock_key_t key = k_spin_lock(&output_lock);
	if (!soft_reset) {
		ring_buf_reset(&output);
		output_dropped = 0;
	}
	k_spin_unlock(&output_lock, key);
	k_thread_create(&application_thread, application_stack,
			K_THREAD_STACK_SIZEOF(application_stack), entry, first, second, third,
			K_LOWEST_APPLICATION_THREAD_PRIO, K_USER, K_FOREVER);
	int err = k_mem_domain_add_thread(&application_domain, &application_thread);
	if (err) {
		k_thread_abort(&application_thread);
		k_thread_join(&application_thread, K_FOREVER);
		status.state = APPLICATION_IDLE;
		status.result = err;
		return err;
	}
	atomic_set(&active, 1);
	k_thread_start(&application_thread);
	return 0;
}

/* lifecycle is held and this loader has passed native_image_check. */
static int run_image(struct llext_loader *loader, size_t length, size_t heap_size)
{
	int err = prepare();
	if (err) {
		return err;
	}
	err = llext_heap_init(execution_memory, heap_size);
	if (err) {
		goto failed;
	}
	loader_initialized = true;
	struct llext_load_param parameters = LLEXT_LOAD_PARAM_DEFAULT;
	err = llext_load(loader, "application", &extension, &parameters);
	if (err) {
		goto failed;
	}
	const void *function = llext_find_sym(&extension->exp_tab, "app_main");
	uintptr_t pc = (uintptr_t)function & ~1U;
	uintptr_t text = (uintptr_t)extension->mem[LLEXT_MEM_TEXT];
	if (!function || pc < text || pc - text >= extension->mem_size[LLEXT_MEM_TEXT]) {
		err = -ENOEXEC;
		goto failed;
	}
	err = llext_add_domain(extension, &application_domain);
	if (err) {
		goto failed;
	}
	err = start_thread(native_entry, extension, (void *)function, NULL,
		(struct application_status){.format = APPLICATION_NATIVE, .image_bytes = length,
			.loader_bytes = heap_size, .loaded_bytes = extension->alloc_size}, false);
	if (err) {
		goto failed;
	}
	return 0;
failed:
	/* Retain the original loading error; a failed release retains ownership. */
	(void)release();
	return err;
}

#ifdef CONFIG_BRICKPHYR_PYTHON
static int run_python(const void *program, size_t length, enum application_format format,
	bool soft_reset)
{
	int err = prepare();
	if (err) {
		return err;
	}
	void *retained = NULL;
	if (program) {
		retained = execution_memory + sizeof(execution_memory) - ROUND_UP(length, 16);
		memcpy(retained, program, length);
	}
	err = k_mem_domain_add_partition(&application_domain, &execution_partition);
	if (!err) {
		err = k_mem_domain_add_partition(&application_domain, &python_partition);
	}
	if (!err) {
		err = start_thread(python_entry, retained, (void *)length, (void *)(uintptr_t)format,
			(struct application_status){.format = format, .image_bytes = length, .repl = !program,
				.loader_bytes = sizeof(execution_memory), .loaded_bytes = sizeof(execution_memory)},
			soft_reset);
	}
	if (!err) {
		return 0;
	}
	(void)release();
	return err;
}

int application_repl(void)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	int err = reap(false);
	if (err == -EAGAIN) {
		err = status.repl ? 0 : -EBUSY;
	} else if (!err && !atomic_get(&active)) {
		err = run_python(NULL, 0, APPLICATION_PYTHON, false);
	}
	k_mutex_unlock(&lifecycle);
	return err;
}
#endif

int application_run_native(const void *image, size_t length)
{
	if (!image || !length) {
		return -EINVAL;
	}
	struct llext_buf_loader loader = LLEXT_TEMPORARY_BUF_LOADER(image, length);
	size_t heap_size;
	int err = native_image_check(&loader.loader, length, &heap_size);
	if (!err) {
		k_mutex_lock(&lifecycle, K_FOREVER);
		err = run_image(&loader.loader, length, heap_size);
		k_mutex_unlock(&lifecycle);
	}
	return err;
}

#ifdef CONFIG_BRICKPHYR_APP_STORAGE
int application_write_upload(const char *path)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	expire_upload();
	if (!upload_heap || (upload_format != APPLICATION_NATIVE && upload_format != APPLICATION_FILES)) {
		k_mutex_unlock(&lifecycle);
		return -ENOEXEC;
	}
	void *image = upload;
	size_t length = upload_size;
	upload = NULL;
	discard_upload();
	k_mutex_unlock(&lifecycle);

	struct fs_file_t file;
	fs_file_t_init(&file);
	int err = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (!err) {
		size_t offset = 0;
		while (offset < length) {
			ssize_t count = fs_write(&file, (const uint8_t *)image + offset, length - offset);
			if (count <= 0) {
				err = count < 0 ? count : -ENOSPC;
				break;
			}
			offset += count;
		}
		int closed = fs_close(&file);
		err = err ? err : closed;
	}
	k_free(image);
	return err;
}

/* The caller's store lock preserves the file during both admission and loading.
 * Native ELF keeps its streaming loader; Python snapshots its complete namespace. */
static int read_stored(const char *path, size_t length, void **image, size_t *heap)
{
	*image = NULL;
	struct llext_fs_loader loader = LLEXT_FS_LOADER(path);
	int err = loader.loader.prepare(&loader.loader);
	if (err) {
		return err;
	}
	uint8_t magic[4];
	err = length < sizeof(magic) ? -ENOEXEC : loader.loader.read(&loader.loader, magic, sizeof(magic));
	if (!err && !memcmp(magic, "\x7f" "ELF", sizeof(magic))) {
		err = native_image_check(&loader.loader, length, heap);
	} else if (!err) {
#ifdef CONFIG_BRICKPHYR_PYTHON
		if (length > APPLICATION_IMAGE_LIMIT) {
			err = -EFBIG;
		} else if (!(*image = k_malloc(length))) {
			err = -ENOMEM;
		} else {
			err = loader.loader.seek(&loader.loader, 0);
			if (!err) {
				err = loader.loader.read(&loader.loader, *image, length);
			}
			if (!err) {
				err = python_image_check(*image, length, APPLICATION_FILES);
			}
		}
#else
		err = -ENOEXEC;
#endif
	}
	loader.loader.finalize(&loader.loader);
	return err;
}

int application_check_file(const char *path, size_t length)
{
	void *image;
	size_t heap;
	int err = read_stored(path, length, &image, &heap);
	k_free(image);
	return err;
}

int application_run_file(const char *path)
{
	struct fs_dirent info;
	int err = fs_stat(path, &info);
	if (err) {
		return err;
	}
	if (info.type != FS_DIR_ENTRY_FILE) {
		return -EINVAL;
	}
	void *image;
	size_t heap;
	err = read_stored(path, info.size, &image, &heap);
	if (!err) {
		k_mutex_lock(&lifecycle, K_FOREVER);
#ifdef CONFIG_BRICKPHYR_PYTHON
		if (image) {
			err = run_python(image, info.size, APPLICATION_FILES, false);
		} else
#endif
		{
			struct llext_fs_loader loader = LLEXT_FS_LOADER(path);
			err = run_image(&loader.loader, info.size, heap);
		}
		k_mutex_unlock(&lifecycle);
	}
	k_free(image);
	return err;
}
#endif

/* The ELF buffer loader carries a 512-byte cache. Keep that stack frame off
 * the Python package/namespace validation path. */
static __noinline int check_native_upload(void)
{
	struct llext_buf_loader loader = LLEXT_TEMPORARY_BUF_LOADER(upload, upload_size);
	return native_image_check(&loader.loader, upload_size, &upload_heap);
}

static __noinline int run_native_upload(void)
{
	struct llext_buf_loader loader = LLEXT_TEMPORARY_BUF_LOADER(upload, upload_size);
	return run_image(&loader.loader, upload_size, upload_heap);
}

static int complete_upload(void)
{
#ifdef CONFIG_BRICKPHYR_PYTHON
	int err;
	if (upload_format == APPLICATION_PYBRICKS) {
		void *image;
		size_t size;
		err = python_pybricks_package(upload, upload_bytes, &image, &size);
		if (err) {
			return err;
		}
		k_free(upload);
		upload = image;
		upload_size = upload_bytes = size;
		upload_format = APPLICATION_FILES;
	}
	if (upload_format != APPLICATION_NATIVE) {
		err = python_image_check(upload, upload_size, upload_format);
		if (!err) {
			upload_heap = sizeof(execution_memory);
		}
		return err;
	}
#endif
	return check_native_upload();
}

#ifdef CONFIG_BRICKPHYR_PYTHON
int application_upload_finish(size_t total)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	expire_upload();
	int err = -EINVAL;
	if (upload && upload_format == APPLICATION_PYBRICKS && !upload_size &&
	    total && total == upload_bytes) {
		upload_size = total;
		err = complete_upload();
		if (err) {
			discard_upload();
		}
	}
	k_mutex_unlock(&lifecycle);
	return err;
}
#endif

int application_upload(enum application_format format, size_t offset, size_t total,
	const void *data, size_t length, size_t *next)
{
	bool open_size = IS_ENABLED(CONFIG_BRICKPHYR_PYTHON) && format == APPLICATION_PYBRICKS && !total;
	size_t bound = total;
#ifdef CONFIG_BRICKPHYR_PYTHON
	if (open_size) {
		bound = PYTHON_PYBRICKS_LIMIT;
	}
#else
	ARG_UNUSED(open_size);
#endif
	if (!data || !next || !length || !bound || offset > bound || length > bound - offset) {
		return -EINVAL;
	}
	if (total > APPLICATION_IMAGE_LIMIT) {
		return -EFBIG;
	}
	if ((unsigned int)format > APPLICATION_PYBRICKS ||
	    (!IS_ENABLED(CONFIG_BRICKPHYR_PYTHON) && format != APPLICATION_NATIVE)) {
		return -ENOTSUP;
	}
	k_mutex_lock(&lifecycle, K_FOREVER);
	expire_upload();
	int err = 0;
	if (shutting_down) {
		err = -ESHUTDOWN;
		goto out;
	}
	if (!offset) {
		/* Allocate before replacing a previous complete image. */
		void *data_buffer = k_malloc(bound);
		if (!data_buffer) {
			err = -ENOMEM;
			goto out;
		}
		discard_upload();
		upload = data_buffer;
		upload_size = total;
		upload_format = format;
		upload_generation = atomic_get(&session_generation);
	}
	if (!upload || total != upload_size || offset > upload_bytes || format != upload_format) {
		err = -EINVAL;
		goto out;
	}
	if (offset < upload_bytes) {
		/* A repeated acknowledged fragment is harmless; a changed one is not. */
		if (length > upload_bytes - offset || memcmp((uint8_t *)upload + offset, data, length)) {
			err = -EINVAL;
			goto out;
		}
	} else {
		memcpy((uint8_t *)upload + offset, data, length);
		upload_bytes += length;
	}
	if (upload_size && upload_bytes == upload_size && !upload_heap) {
		err = complete_upload();
		if (err) {
			discard_upload();
			goto out;
		}
	}
	*next = upload_bytes;
out:
	k_mutex_unlock(&lifecycle);
	return err;
}

int application_run_uploaded(bool retain)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	expire_upload();
	int err = -ENODATA;
	if (upload_heap) {
#ifdef CONFIG_BRICKPHYR_PYTHON
		if (upload_format != APPLICATION_NATIVE) {
			err = run_python(upload, upload_size, upload_format, false);
		} else
#endif
		{
			err = run_native_upload();
		}
		if (!err && !retain) {
			/* The temporary buffer loader copies all retained sections. */
			discard_upload();
		}
	}
	k_mutex_unlock(&lifecycle);
	return err;
}

/* Caller holds lifecycle; reading output state never consumes it. */
static void copy_status(struct application_status *value)
{
	*value = status;
	value->upload_bytes = upload_bytes;
	value->upload_size = upload_size;
	value->upload_format = upload_format;
	value->upload_ready = upload_heap != 0;
	k_spinlock_key_t key = k_spin_lock(&output_lock);
	value->output_dropped = output_dropped;
	value->output_bytes = ring_buf_size_get(&output);
	k_spin_unlock(&output_lock, key);
}

int application_observe(struct application_status *value)
{
	if (k_mutex_lock(&lifecycle, K_NO_WAIT)) {
		return -EBUSY;
	}
	copy_status(value);
	k_mutex_unlock(&lifecycle);
	return 0;
}

void application_get_status(struct application_status *value)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	(void)reap(false);
	expire_upload();
	copy_status(value);
	k_mutex_unlock(&lifecycle);
}

size_t application_output(void *data, size_t capacity)
{
	k_spinlock_key_t key = k_spin_lock(&output_lock);
	size_t count = ring_buf_get(&output, data, capacity);
	k_spin_unlock(&output_lock, key);
#ifdef CONFIG_BRICKPHYR_PYTHON
	if (count) {
		k_sem_give(&io_ready);
	}
#endif
	return count;
}

static int cmd_app(const struct shell *shell, size_t argc, char **argv)
{
#ifdef CONFIG_BRICKPHYR_PYTHON
	if (argc >= 2 && !strcmp(argv[1], "input")) {
		size_t length = argc == 3 ? strlen(argv[2]) : 0;
		if (length >= 128) {
			return -EMSGSIZE;
		}
		char line[128];
		if (length) {
			memcpy(line, argv[2], length);
		}
		line[length++] = '\n';
		int err = application_input(line, length);
		shell_print(shell, "Input: %d", err);
		return err;
	}
	if (argc == 2 && !strcmp(argv[1], "interrupt")) {
		int err = application_interrupt();
		shell_print(shell, "Interrupt: %d", err);
		return err;
	}
#endif
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
	if (argc == 3 && (!strcmp(argv[1], "run") || !strcmp(argv[1], "remove"))) {
		int err = !strcmp(argv[1], "run") ? app_store_run(argv[2]) : app_store_remove(argv[2]);
		shell_print(shell, "%s: %d", argv[1], err);
		return err;
	}
	if (argc == 2 && !strcmp(argv[1], "list")) {
		struct app_file entry = {0};
		size_t available;
		for (;;) {
			char after[sizeof(entry.name)];
			strcpy(after, entry.name);
			int err = app_store_next(after, &entry, &available);
			if (err) {
				shell_error(shell, "List: %d", err);
				return err;
			}
			if (!entry.name[0]) {
				shell_print(shell, "Available: %zu bytes", available);
				return 0;
			}
			shell_print(shell, "%s: %zu bytes", entry.name, entry.size);
		}
	}
#endif

	if (argc == 2 && !strcmp(argv[1], "run")) {
		int err = application_run_uploaded(false);
		shell_print(shell, "Run: %d", err);
		return err;
	}
	if (argc == 2 && !strcmp(argv[1], "discard")) {
		application_discard_upload();
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "stop")) {
		int err = application_stop();
		shell_print(shell, "Stop: %d", err);
		return err;
	}
	if (argc == 2 && !strcmp(argv[1], "output")) {
		char data[129];
		size_t count;
		size_t remaining = OUTPUT_CAPACITY;
		while (remaining && (count = application_output(data, MIN(sizeof(data) - 1, remaining)))) {
			data[count] = 0;
			shell_fprintf(shell, SHELL_NORMAL, "%s", data);
			remaining -= count;
		}
		return 0;
	}
	if (argc != 1 && (argc != 2 || strcmp(argv[1], "status"))) {
		shell_error(shell, "Usage: app [status | run [name] | list | remove name | stop | output | discard"
#ifdef CONFIG_BRICKPHYR_PYTHON
			" | input [text] | interrupt"
#endif
			"]");
		return -EINVAL;
	}
	struct application_status value;
	application_get_status(&value);
	static const char *const states[] = {"idle", "running", "returned", "stopped", "fault"};
	shell_print(shell, "Application: %s; result %d; fault %u; cleanup %d", states[value.state],
		    value.result, value.fault, value.cleanup_error);
	static const char *const formats[] = {"native", "python", "mpy", "files"};
	shell_print(shell, "Format: %s", formats[value.format]);
	shell_print(shell, "Image %zu; loader budget %zu; loaded %zu; stack %zu (unused %zu)",
		    value.image_bytes, value.loader_bytes, value.loaded_bytes,
		    value.stack_bytes, value.stack_unused);
	if (value.code_bytes) {
		shell_print(shell, "Python native code: %zu/%zu bytes", value.code_used, value.code_bytes);
	}
	shell_print(shell, "Output dropped: %u; ABI %s", value.output_dropped, BRICKPHYR_ABI);
	shell_print(shell, "Upload %zu/%zu; %s", value.upload_bytes, value.upload_size,
		    !value.upload_size ? "empty" : value.upload_ready ? "ready" : "incomplete");
	return 0;
}

SHELL_CMD_ARG_REGISTER(app, NULL, "Application lifecycle and I/O.", cmd_app, 1, 2);
