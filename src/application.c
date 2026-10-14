// SPDX-License-Identifier: Apache-2.0
#include "application.h"
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
#include "app_store.h"
#include "app_data.h"
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
#include "hub_radio.h"
#include "hub_ble_peer.h"
#include "hub_network.h"
#include "hub_sound.h"
#include "hub_imu.h"
#include "hub_battery.h"
#include "hub_charger.h"
#include "hub_ports.h"
#include "control_frame.h"
#include "application_record.h"
#include "uart_settings.h"
#include "hub_power.h"
#include "hub_reset.h"

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
#include <zephyr/sys/kobject.h>

/* A timer wakeup can reset equal-priority time slicing. Control and diagnostics
 * must preempt the application, even when its entry never yields. */
BUILD_ASSERT(CONFIG_SHELL_THREAD_PRIORITY_OVERRIDE &&
	     CONFIG_SHELL_THREAD_PRIORITY < K_LOWEST_APPLICATION_THREAD_PRIO);
BUILD_ASSERT(CONFIG_LOG_PROCESS_THREAD_CUSTOM_PRIORITY &&
	     CONFIG_LOG_PROCESS_THREAD_PRIORITY < K_LOWEST_APPLICATION_THREAD_PRIO);
BUILD_ASSERT(CONFIG_DYNAMIC_THREAD_POOL_SIZE == 0,
	"Application stacks must not reserve a static dynamic-thread pool");

static FUNC_NORETURN void user_memory_fault(const char *operation, const void *data,
	size_t size, const char *reason)
{
	LOG_MODULE_DECLARE(os, CONFIG_KERNEL_LOG_LEVEL);
	LOG_ERR("syscall %s: memory region %p (size %zu) %s", operation, data, size, reason);
	K_OOPS(true);
}

/* Outline only the cold fault path. Constant size/alignment/access checks
 * still fold at each caller, without another call or stack frame on success. */
static ALWAYS_INLINE void user_memory_check(const void *data, size_t size, size_t alignment,
	bool write, const char *operation)
{
	if (!IS_ALIGNED(data, alignment)) {
		user_memory_fault(operation, data, size, "unaligned");
	}
	/* Use the same overflow guards and MPU check as K_SYSCALL_MEMORY. */
	if (!K_SYSCALL_MEMORY_SIZE_CHECK(data, size) || Z_DETECT_POINTER_OVERFLOW(data, size) ||
	    arch_buffer_validate((void *)data, size, write) != 0) {
		user_memory_fault(operation, data, size, write ? "write access denied" : "read access denied");
	}
}

K_MUTEX_DEFINE(lifecycle);
/* The supervisor reserves the stack before replacing the current application. */
static k_thread_stack_t *application_stack;
static size_t application_stack_size;
static struct k_thread application_thread;
static struct k_mem_domain application_domain;
static bool domain_initialized;
static struct llext *extension;
static bool loader_initialized;
/* One native image, standalone or alongside Python, with its own backing. */
static void *native_memory;
static struct k_spinlock control_lock;
static struct {
	struct bp_control_step step;
	int64_t next_tick;
	uint32_t period_us;
} control_clock;
#ifdef CONFIG_BRICKPHYR_PYTHON
/* The user-mode entry may read VM-owned launch data, not supervisor pointers. */
K_APP_BMEM(python_partition) static uintptr_t python_stack_bottom;
/* Only Python maps this arena. A controller never receives VM pointers. */
static uint8_t execution_memory[APPLICATION_MEMORY_LIMIT]
	__aligned(APPLICATION_MEMORY_LIMIT)
	Z_GENERIC_SECTION(.brickphyr_execution);
K_MEM_PARTITION_DEFINE(execution_partition, execution_memory, sizeof(execution_memory),
	K_MEM_PARTITION_P_RW_U_RW);
static struct native_control {
	struct k_mem_domain domain;
	struct k_thread *thread;
	k_thread_stack_t *stack;
	size_t stack_size, parameter_size, loader_size;
	uint32_t revision;
	bool domain_initialized, started;
	uint8_t parameters[];
} *controller;
static struct {
	const void *image, *parameters;
	size_t size, length;
	int result;
} control_request;
static atomic_t control_pending;
static K_SEM_DEFINE(control_done, 0, 1);
static int start_controller(void);
BUILD_ASSERT(CONFIG_SHELL_THREAD_PRIORITY < K_LOWEST_APPLICATION_THREAD_PRIO - 1);
BUILD_ASSERT(CONFIG_LOG_PROCESS_THREAD_PRIORITY < K_LOWEST_APPLICATION_THREAD_PRIO - 1);
#endif
/* ARMv8-M bounds dynamic regions at app_smem; this earlier arena would require
 * a corresponding region-boundary integration before using that MPU model. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_MPU_REQUIRES_NON_OVERLAPPING_REGIONS));
static struct application_status status;
static atomic_t return_value;
static atomic_t fault_reason;
static atomic_t active;
static unsigned int finished_source;
#define OUTPUT_CAPACITY 1024
RING_BUF_DECLARE(output, OUTPUT_CAPACITY);
static struct k_spinlock output_lock;
static uint32_t output_dropped;
static bool output_attached;
static void *upload;
static size_t upload_capacity;
static size_t upload_size;
static size_t upload_bytes;
static struct native_image_requirements upload_requirements;
static enum application_format upload_format;
static bool upload_pybricks;
static atomic_t session_generation;
static atomic_val_t upload_generation;
static bool shutting_down;
static struct k_spinlock button_lock;
static uint32_t stop_buttons;
static uint32_t observed_buttons;
static bool stop_pressed;
static struct bp_record_spec record_request;
static atomic_t record_pending;
static int record_result;
static K_SEM_DEFINE(record_done, 0, 1);

static bool native_caller(void)
{
	struct k_thread *thread = k_sched_current_thread_query();
#ifdef CONFIG_BRICKPHYR_PYTHON
	if (controller && controller->started && thread == controller->thread) return true;
#endif
	return atomic_get(&active) && status.format == APPLICATION_NATIVE && thread == &application_thread;
}

static enum bp_port_owner application_port_owner(void)
{
#ifdef CONFIG_BRICKPHYR_PYTHON
	if (controller && k_sched_current_thread_query() == controller->thread) return BP_PORT_CONTROLLER;
#endif
	return BP_PORT_APPLICATION;
}

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
	bool soft_reset, int program_id);

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
static K_SEM_DEFINE(code_ready, 0, 1);
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

static void code_worker(void *a, void *b, void *c)
{
	/* The application is blocked. This privileged worker has its own MPU map:
	 * it can fill new code while the application's region stays read/execute.
	 * Serializing with stop prevents copying from an unloaded application. */
	for (;;) {
		k_sem_take(&code_ready, K_FOREVER);
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
}
/* Initializing the 16 KiB arena exceeds one IMU period on F4. Keep it
 * preemptible below hardware and management, with the blocked application's
 * priority. */
K_KERNEL_THREAD_DEFINE(code_thread, 1024, code_worker, NULL, NULL, NULL,
	K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static intptr_t code_exchange(bool reserve, const void *data, size_t length)
{
	code_request.reserve = reserve;
	code_request.data = data;
	code_request.length = length;
	atomic_set(&code_pending, 1);
	k_sem_give(&code_ready);
	k_sem_take(&code_done, K_FOREVER);
	return code_request.result;
}

static void verify_python_thread(void)
{
	K_OOPS(K_SYSCALL_VERIFY(atomic_get(&active) &&
		k_sched_current_thread_query() == &application_thread &&
		status.format != APPLICATION_NATIVE));
}

/* Host callbacks never retain or dereference a Python-heap pointer. This one
 * bounded mailbox belongs to the supervisor, including forced-stop cleanup. */
static struct k_spinlock message_lock;
/* RX is zeroed on open; TX is filled before publication. Keep these buffers
 * with noinit storage, avoiding a large MPU stack-alignment gap after BSS. */
static __noinit uint8_t message_rx[APPLICATION_MESSAGE_RX_MAX];
static __noinit uint8_t message_tx[APPLICATION_MESSAGE_TX_MAX];
static struct {
	size_t rx_size, tx_size, capacity;
	uint32_t sequence, lease, request;
	int result;
} message;

/* Positive syscall results carry tokens, negative results carry errno. */
static uint32_t message_token(void)
{
	message.sequence = message.sequence % INT32_MAX + 1;
	return message.sequence;
}

int z_impl_application_message_open(size_t capacity)
{
	if (capacity > sizeof(message_rx)) return -EMSGSIZE;
	k_spinlock_key_t key = k_spin_lock(&message_lock);
	int result = -EBUSY;
	if (!message.lease) {
		memset(message_rx, 0, capacity);
		message.rx_size = capacity;
		message.tx_size = message.request = 0;
		message.result = 0;
		result = message.lease = message_token();
	}
	k_spin_unlock(&message_lock, key);
	return result;
}
static inline int z_vrfy_application_message_open(size_t capacity)
{
	verify_python_thread();
	return z_impl_application_message_open(capacity);
}
#include <zephyr/syscalls/application_message_open_mrsh.c>

int z_impl_application_message_close(uint32_t lease)
{
	k_spinlock_key_t key = k_spin_lock(&message_lock);
	if (lease && message.lease == lease) {
		message.lease = message.request = 0;
		message.tx_size = message.rx_size = 0;
	}
	k_spin_unlock(&message_lock, key);
	return 0;
}
static inline int z_vrfy_application_message_close(uint32_t lease)
{
	verify_python_thread();
	return z_impl_application_message_close(lease);
}
#include <zephyr/syscalls/application_message_close_mrsh.c>

int z_impl_application_message_read(uint32_t lease, size_t offset, void *data, size_t length)
{
	k_spinlock_key_t key = k_spin_lock(&message_lock);
	int err = !lease || message.lease != lease ? -EBADF :
		offset > message.rx_size || length > message.rx_size - offset ? -EINVAL : 0;
	if (!err && length) memcpy(data, message_rx + offset, length);
	k_spin_unlock(&message_lock, key);
	return err;
}
static inline int z_vrfy_application_message_read(uint32_t lease, size_t offset, void *data, size_t length)
{
	verify_python_thread();
	if (length > sizeof(message_rx)) return -EMSGSIZE;
	if (length) user_memory_check(data, length, 1, true, __func__);
	return z_impl_application_message_read(lease, offset, data, length);
}
#include <zephyr/syscalls/application_message_read_mrsh.c>

int z_impl_application_message_send(uint32_t lease, const void *data, size_t length)
{
	k_spinlock_key_t key = k_spin_lock(&message_lock);
	int result = !lease || message.lease != lease ? -EBADF :
		!message.capacity ? -ENOTCONN : length > message.capacity ? -EMSGSIZE : 0;
	if (!result) {
		if (length) memcpy(message_tx, data, length);
		message.tx_size = length;
		message.result = length ? -EAGAIN : 0;
		result = message.request = message_token();
	}
	k_spin_unlock(&message_lock, key);
	return result;
}
static inline int z_vrfy_application_message_send(uint32_t lease, const void *data, size_t length)
{
	verify_python_thread();
	if (length > sizeof(message_tx)) return -EMSGSIZE;
	if (length) user_memory_check(data, length, 1, false, __func__);
	return z_impl_application_message_send(lease, data, length);
}
#include <zephyr/syscalls/application_message_send_mrsh.c>

int z_impl_application_message_status(uint32_t lease, uint32_t request)
{
	k_spinlock_key_t key = k_spin_lock(&message_lock);
	int result = !lease || message.lease != lease ? -EBADF :
		!request || request != message.request ? -ECANCELED : message.result;
	k_spin_unlock(&message_lock, key);
	return result;
}
static inline int z_vrfy_application_message_status(uint32_t lease, uint32_t request)
{
	verify_python_thread();
	return z_impl_application_message_status(lease, request);
}
#include <zephyr/syscalls/application_message_status_mrsh.c>

int z_impl_application_message_cancel(uint32_t lease, uint32_t request)
{
	k_spinlock_key_t key = k_spin_lock(&message_lock);
	if (lease && lease == message.lease && request && request == message.request &&
	    message.result == -EAGAIN) {
		message.result = -ECANCELED;
	}
	k_spin_unlock(&message_lock, key);
	return 0;
}
static inline int z_vrfy_application_message_cancel(uint32_t lease, uint32_t request)
{
	verify_python_thread();
	return z_impl_application_message_cancel(lease, request);
}
#include <zephyr/syscalls/application_message_cancel_mrsh.c>

void application_message_attach(size_t capacity)
{
	k_spinlock_key_t key = k_spin_lock(&message_lock);
	bool changed = message.capacity != MIN(capacity, sizeof(message_tx));
	message.capacity = MIN(capacity, sizeof(message_tx));
	if (message.result == -EAGAIN && (!capacity || message.tx_size > capacity)) {
		message.result = capacity ? -EMSGSIZE : -ENOTCONN;
	}
	k_spin_unlock(&message_lock, key);
	if (changed) k_sem_give(&io_ready);
}

int application_message_receive(size_t offset, const void *data, size_t length)
{
	k_spinlock_key_t key = k_spin_lock(&message_lock);
	/* The reference ignores writes when no AppData object is listening. */
	int err = message.lease && (!length || offset > message.rx_size ||
		length > message.rx_size - offset) ? -EINVAL : 0;
	if (message.lease && !err) memcpy(message_rx + offset, data, length);
	k_spin_unlock(&message_lock, key);
	if (!err) k_sem_give(&io_ready);
	return err;
}

size_t application_message_event(void *data, size_t capacity, uint32_t *request)
{
	k_spinlock_key_t key = k_spin_lock(&message_lock);
	size_t size = 0;
	bool failed = false;
	if (message.lease && message.result == -EAGAIN && message.capacity) {
		if (message.tx_size > capacity) {
			message.result = -EMSGSIZE;
			failed = true;
		} else {
			size = message.tx_size;
			memcpy(data, message_tx, size);
			*request = message.request;
		}
	}
	k_spin_unlock(&message_lock, key);
	if (failed) k_sem_give(&io_ready);
	return size;
}

bool application_message_pending(uint32_t request)
{
	k_spinlock_key_t key = k_spin_lock(&message_lock);
	bool pending = message.lease && request == message.request && message.result == -EAGAIN;
	k_spin_unlock(&message_lock, key);
	return pending;
}

void application_message_sent(uint32_t request)
{
	k_spinlock_key_t key = k_spin_lock(&message_lock);
	if (message.lease && request == message.request && message.result == -EAGAIN) {
		message.result = 0;
	}
	k_spin_unlock(&message_lock, key);
	k_sem_give(&io_ready);
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
		user_memory_check(data, capacity, 1, true, __func__);
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
		user_memory_check(data, length, 1, false, __func__);
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
		user_memory_check(data, length, 1, false, __func__);
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
	upload_capacity = upload_size = upload_bytes = 0;
	upload_requirements = (struct native_image_requirements){0};
}

static void expire_upload(void)
{
	if (!upload_requirements.heap_size && upload_generation != atomic_get(&session_generation)) {
		discard_upload();
	}
}

void application_session_closed(void)
{
	atomic_inc(&session_generation);
#ifdef CONFIG_BRICKPHYR_PYTHON
	application_output_stream(false);
	application_message_attach(0);
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
	user_memory_check(data, length, 1, false, __func__);
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

/* Keep optional calls in the ABI/SVC table, but omit buffer validation and
 * copies when the service is absent. Enabled services still validate before
 * entering their locks; device readiness is not a reason to skip validation. */
static inline int z_vrfy_bp_light_write(uint32_t light, const uint8_t *rgb)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_LIGHT)) return -ENOTSUP;
	user_memory_check(rgb, 3, 1, false, __func__);
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
	if (!IS_ENABLED(CONFIG_BRICKPHYR_LIGHT)) return -ENOTSUP;
	user_memory_check(frames, count * sizeof(*frames), __alignof__(*frames), false, __func__);
	return z_impl_bp_light_animate(light, frames, count);
}
#include <zephyr/syscalls/bp_light_animate_mrsh.c>

int z_impl_bp_light_get(uint32_t light, struct bp_light_state *state)
{
	return hub_light_get(light, state);
}

static inline int z_vrfy_bp_light_get(uint32_t light, struct bp_light_state *state)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_LIGHT)) return -ENOTSUP;
	user_memory_check(state, sizeof(*state), __alignof__(*state), true, __func__);
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
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MATRIX)) return -ENOTSUP;
	user_memory_check(values, count, 1, false, __func__);
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
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MATRIX)) return -ENOTSUP;
	user_memory_check(frames, count * BP_MATRIX_PIXELS, 1, false, __func__);
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
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MATRIX)) return -ENOTSUP;
	/* Range validation alone permits misaligned memory. A resulting ARM
	 * fault inside the snapshot copy would strand the service's spinlock. */
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_matrix_get(value);
}
#include <zephyr/syscalls/bp_matrix_get_mrsh.c>

int z_impl_bp_imu_get(struct bp_imu_sample *value)
{
	return hub_imu_get(value);
}

static inline int z_vrfy_bp_imu_get(struct bp_imu_sample *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_IMU)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_imu_get(value);
}
#include <zephyr/syscalls/bp_imu_get_mrsh.c>

int z_impl_bp_imu_get_state(struct bp_imu_state *value)
{
	return hub_imu_get_state(value);
}

static inline int z_vrfy_bp_imu_get_state(struct bp_imu_state *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_IMU)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_imu_get_state(value);
}
#include <zephyr/syscalls/bp_imu_get_state_mrsh.c>

int z_impl_bp_imu_configure(const struct bp_imu_command *command)
{
	if (hub_drivebase_uses_gyro()) return -EBUSY;
	return hub_imu_configure(command);
}

static inline int z_vrfy_bp_imu_configure(const struct bp_imu_command *command)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_IMU)) return -ENOTSUP;
	user_memory_check(command, sizeof(*command), __alignof__(*command), false, __func__);
	return z_impl_bp_imu_configure(command);
}
#include <zephyr/syscalls/bp_imu_configure_mrsh.c>

int z_impl_bp_imu_get_application(struct bp_imu_application_state *value)
{
	return hub_imu_get_application(value);
}

static inline int z_vrfy_bp_imu_get_application(struct bp_imu_application_state *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_IMU)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_imu_get_application(value);
}
#include <zephyr/syscalls/bp_imu_get_application_mrsh.c>

int z_impl_bp_battery_get(struct bp_battery_sample *value)
{
	return hub_battery_get(value);
}
static inline int z_vrfy_bp_battery_get(struct bp_battery_sample *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_BATTERY)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_battery_get(value);
}
#include <zephyr/syscalls/bp_battery_get_mrsh.c>

int z_impl_bp_charger_get(struct bp_charger_state *value)
{
	return hub_charger_get(value);
}
static inline int z_vrfy_bp_charger_get(struct bp_charger_state *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_CHARGER)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_charger_get(value);
}
#include <zephyr/syscalls/bp_charger_get_mrsh.c>

int z_impl_bp_port_get(uint32_t port, struct bp_port_state *value)
{
	return hub_port_get(port, value);
}
static inline int z_vrfy_bp_port_get(uint32_t port, struct bp_port_state *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORTS)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_port_get(port, value);
}
#include <zephyr/syscalls/bp_port_get_mrsh.c>

int z_impl_bp_radio_observe(const uint8_t *channels, uint32_t count, uint32_t *handle)
{
	return hub_radio_observe(channels, count, handle);
}
static inline int z_vrfy_bp_radio_observe(const uint8_t *channels, uint32_t count, uint32_t *handle)
{
	if (count > BP_RADIO_CHANNELS_MAX) return -EINVAL;
	if (!IS_ENABLED(CONFIG_BRICKPHYR_RADIO)) return -ENOTSUP;
	if (count) user_memory_check(channels, count, 1, false, __func__);
	user_memory_check(handle, sizeof(*handle), __alignof__(*handle), true, __func__);
	return z_impl_bp_radio_observe(channels, count, handle);
}
#include <zephyr/syscalls/bp_radio_observe_mrsh.c>

int z_impl_bp_radio_ready(uint32_t handle) { return hub_radio_ready(handle); }
static inline int z_vrfy_bp_radio_ready(uint32_t handle) { return z_impl_bp_radio_ready(handle); }
#include <zephyr/syscalls/bp_radio_ready_mrsh.c>

int z_impl_bp_radio_version(char *value, uint32_t size) { return hub_radio_version(value, size); }
static inline int z_vrfy_bp_radio_version(char *value, uint32_t size)
{
	if (!size || size > BP_RADIO_VERSION_MAX) return -EINVAL;
	if (!IS_ENABLED(CONFIG_BRICKPHYR_RADIO)) return -ENOTSUP;
	user_memory_check(value, size, 1, true, __func__);
	return z_impl_bp_radio_version(value, size);
}
#include <zephyr/syscalls/bp_radio_version_mrsh.c>

int z_impl_bp_radio_get(uint32_t handle, uint32_t channel, struct bp_radio_sample *sample)
{
	return hub_radio_get(handle, channel, sample);
}
static inline int z_vrfy_bp_radio_get(uint32_t handle, uint32_t channel, struct bp_radio_sample *sample)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_RADIO)) return -ENOTSUP;
	user_memory_check(sample, sizeof(*sample), __alignof__(*sample), true, __func__);
	return z_impl_bp_radio_get(handle, channel, sample);
}
#include <zephyr/syscalls/bp_radio_get_mrsh.c>

int z_impl_bp_radio_close(uint32_t handle) { return hub_radio_close(handle); }
static inline int z_vrfy_bp_radio_close(uint32_t handle) { return z_impl_bp_radio_close(handle); }
#include <zephyr/syscalls/bp_radio_close_mrsh.c>

int z_impl_bp_ble_peer_open(const struct bp_ble_peer_config *config, uint32_t *handle)
{
	return hub_ble_peer_open(config, handle);
}
static inline int z_vrfy_bp_ble_peer_open(const struct bp_ble_peer_config *config, uint32_t *handle)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_BLE_PEER)) return -ENOTSUP;
	user_memory_check(config, sizeof(*config), __alignof__(*config), false, __func__);
	user_memory_check(handle, sizeof(*handle), __alignof__(*handle), true, __func__);
	return z_impl_bp_ble_peer_open(config, handle);
}
#include <zephyr/syscalls/bp_ble_peer_open_mrsh.c>

int z_impl_bp_ble_peer_connect(uint32_t handle) { return hub_ble_peer_connect(handle); }
static inline int z_vrfy_bp_ble_peer_connect(uint32_t handle) { return z_impl_bp_ble_peer_connect(handle); }
#include <zephyr/syscalls/bp_ble_peer_connect_mrsh.c>

int z_impl_bp_ble_peer_disconnect(uint32_t handle) { return hub_ble_peer_disconnect(handle); }
static inline int z_vrfy_bp_ble_peer_disconnect(uint32_t handle) { return z_impl_bp_ble_peer_disconnect(handle); }
#include <zephyr/syscalls/bp_ble_peer_disconnect_mrsh.c>

int z_impl_bp_ble_peer_close(uint32_t handle) { return hub_ble_peer_close(handle); }
static inline int z_vrfy_bp_ble_peer_close(uint32_t handle) { return z_impl_bp_ble_peer_close(handle); }
#include <zephyr/syscalls/bp_ble_peer_close_mrsh.c>

int z_impl_bp_ble_peer_get(uint32_t handle, struct bp_ble_peer_state *state) { return hub_ble_peer_get(handle, state); }
static inline int z_vrfy_bp_ble_peer_get(uint32_t handle, struct bp_ble_peer_state *state)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_BLE_PEER)) return -ENOTSUP;
	user_memory_check(state, sizeof(*state), __alignof__(*state), true, __func__);
	return z_impl_bp_ble_peer_get(handle, state);
}
#include <zephyr/syscalls/bp_ble_peer_get_mrsh.c>

int z_impl_bp_network_open(uint32_t size, uint32_t *handle) { return hub_network_open(size, handle); }
static inline int z_vrfy_bp_network_open(uint32_t size, uint32_t *handle)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_HUB_NETWORK)) return -ENOTSUP;
	user_memory_check(handle, sizeof(*handle), __alignof__(*handle), true, __func__);
	return z_impl_bp_network_open(size, handle);
}
#include <zephyr/syscalls/bp_network_open_mrsh.c>

int z_impl_bp_network_get(uint32_t handle, struct bp_network_state *state) { return hub_network_get(handle, state); }
static inline int z_vrfy_bp_network_get(uint32_t handle, struct bp_network_state *state)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_HUB_NETWORK)) return -ENOTSUP;
	user_memory_check(state, sizeof(*state), __alignof__(*state), true, __func__);
	return z_impl_bp_network_get(handle, state);
}
#include <zephyr/syscalls/bp_network_get_mrsh.c>

int z_impl_bp_network_connect(uint32_t handle, const uint8_t *address) { return hub_network_connect(handle, address); }
static inline int z_vrfy_bp_network_connect(uint32_t handle, const uint8_t *address)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_HUB_NETWORK)) return -ENOTSUP;
	user_memory_check(address, 6, 1, false, __func__);
	return z_impl_bp_network_connect(handle, address);
}
#include <zephyr/syscalls/bp_network_connect_mrsh.c>

int z_impl_bp_network_send(uint32_t handle, const struct bp_network_message *message) { return hub_network_send(handle, message); }
static inline int z_vrfy_bp_network_send(uint32_t handle, const struct bp_network_message *message)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_HUB_NETWORK)) return -ENOTSUP;
	user_memory_check(message, sizeof(*message), __alignof__(*message), false, __func__);
	return z_impl_bp_network_send(handle, message);
}
#include <zephyr/syscalls/bp_network_send_mrsh.c>

int z_impl_bp_network_cancel(uint32_t handle, uint32_t request) { return hub_network_cancel(handle, request); }
static inline int z_vrfy_bp_network_cancel(uint32_t handle, uint32_t request)
{
	return z_impl_bp_network_cancel(handle, request);
}
#include <zephyr/syscalls/bp_network_cancel_mrsh.c>

int z_impl_bp_network_snapshot(uint32_t handle, uint32_t latest, uint32_t *through) { return hub_network_snapshot(handle, latest, through); }
static inline int z_vrfy_bp_network_snapshot(uint32_t handle, uint32_t latest, uint32_t *through)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_HUB_NETWORK)) return -ENOTSUP;
	user_memory_check(through, sizeof(*through), __alignof__(*through), true, __func__);
	return z_impl_bp_network_snapshot(handle, latest, through);
}
#include <zephyr/syscalls/bp_network_snapshot_mrsh.c>

int z_impl_bp_network_read(uint32_t handle, uint32_t through, struct bp_network_message *message) { return hub_network_read(handle, through, message); }
static inline int z_vrfy_bp_network_read(uint32_t handle, uint32_t through, struct bp_network_message *message)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_HUB_NETWORK)) return -ENOTSUP;
	user_memory_check(message, sizeof(*message), __alignof__(*message), true, __func__);
	return z_impl_bp_network_read(handle, through, message);
}
#include <zephyr/syscalls/bp_network_read_mrsh.c>

int z_impl_bp_network_close(uint32_t handle) { return hub_network_close(handle); }
static inline int z_vrfy_bp_network_close(uint32_t handle)
{
	return z_impl_bp_network_close(handle);
}
#include <zephyr/syscalls/bp_network_close_mrsh.c>

int z_impl_bp_xbox_rumble(uint32_t handle, const struct bp_xbox_rumble *rumble)
{
	return hub_xbox_rumble(handle, rumble);
}
static inline int z_vrfy_bp_xbox_rumble(uint32_t handle, const struct bp_xbox_rumble *rumble)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_BLE_PEER)) return -ENOTSUP;
	user_memory_check(rumble, sizeof(*rumble), __alignof__(*rumble), false, __func__);
	return z_impl_bp_xbox_rumble(handle, rumble);
}
#include <zephyr/syscalls/bp_xbox_rumble_mrsh.c>

int z_impl_bp_lwp3_write(uint32_t handle, const void *data, uint32_t size)
{
	return hub_lwp3_write(handle, data, size);
}
static inline int z_vrfy_bp_lwp3_write(uint32_t handle, const void *data, uint32_t size)
{
	if (size < 3 || size > BP_LWP3_MESSAGE_MAX) return -EINVAL;
	if (!IS_ENABLED(CONFIG_BRICKPHYR_BLE_PEER)) return -ENOTSUP;
	user_memory_check(data, size, 1, false, __func__);
	return z_impl_bp_lwp3_write(handle, data, size);
}
#include <zephyr/syscalls/bp_lwp3_write_mrsh.c>

int z_impl_bp_lwp3_read(uint32_t handle, struct bp_lwp3_message *message)
{
	return hub_lwp3_read(handle, message);
}
static inline int z_vrfy_bp_lwp3_read(uint32_t handle, struct bp_lwp3_message *message)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_BLE_PEER)) return -ENOTSUP;
	user_memory_check(message, sizeof(*message), 1, true, __func__);
	return z_impl_bp_lwp3_read(handle, message);
}
#include <zephyr/syscalls/bp_lwp3_read_mrsh.c>

int z_impl_bp_lwp3_command(uint32_t handle, uint32_t command, int32_t value, int32_t auxiliary)
{
	return hub_lwp3_command(handle, command, value, auxiliary);
}
static inline int z_vrfy_bp_lwp3_command(uint32_t handle, uint32_t command, int32_t value, int32_t auxiliary)
{
	return z_impl_bp_lwp3_command(handle, command, value, auxiliary);
}
#include <zephyr/syscalls/bp_lwp3_command_mrsh.c>

int z_impl_bp_lwp3_palette_get(uint32_t handle, struct bp_sensor_palette *palette)
{
	return hub_lwp3_palette_get(handle, palette);
}
static inline int z_vrfy_bp_lwp3_palette_get(uint32_t handle, struct bp_sensor_palette *palette)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_BLE_PEER)) return -ENOTSUP;
	user_memory_check(palette, sizeof(*palette), __alignof__(*palette), true, __func__);
	return z_impl_bp_lwp3_palette_get(handle, palette);
}
#include <zephyr/syscalls/bp_lwp3_palette_get_mrsh.c>

int z_impl_bp_lwp3_palette_set(uint32_t handle, const struct bp_sensor_palette *palette)
{
	return hub_lwp3_palette_set(handle, palette);
}
static inline int z_vrfy_bp_lwp3_palette_set(uint32_t handle, const struct bp_sensor_palette *palette)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_BLE_PEER)) return -ENOTSUP;
	user_memory_check(palette, sizeof(*palette), __alignof__(*palette), false, __func__);
	return z_impl_bp_lwp3_palette_set(handle, palette);
}
#include <zephyr/syscalls/bp_lwp3_palette_set_mrsh.c>

int z_impl_bp_port_get_mode(uint32_t port, uint32_t generation, uint32_t mode,
	struct bp_port_mode *value)
{
	return hub_port_get_mode(port, generation, mode, value);
}
static inline int z_vrfy_bp_port_get_mode(uint32_t port, uint32_t generation, uint32_t mode,
	struct bp_port_mode *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_port_get_mode(port, generation, mode, value);
}
#include <zephyr/syscalls/bp_port_get_mode_mrsh.c>

int z_impl_bp_port_select(uint32_t port, uint32_t generation, uint32_t mode)
{
	return hub_port_select(application_port_owner(), port, generation, mode);
}
static inline int z_vrfy_bp_port_select(uint32_t port, uint32_t generation, uint32_t mode)
{
	return z_impl_bp_port_select(port, generation, mode);
}
#include <zephyr/syscalls/bp_port_select_mrsh.c>

int z_impl_bp_port_write(uint32_t port, uint32_t generation, uint32_t mode,
	const uint8_t *data, uint32_t size, uint32_t *request)
{
	return hub_port_write(application_port_owner(), port, generation, mode, data, size, request);
}
static inline int z_vrfy_bp_port_write(uint32_t port, uint32_t generation, uint32_t mode,
	const uint8_t *data, uint32_t size, uint32_t *request)
{
	if (!size || size > 32) return -EINVAL;
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(data, size, 1, false, __func__);
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	uint8_t copy[32];
	memcpy(copy, data, size);
	return z_impl_bp_port_write(port, generation, mode, copy, size, request);
}
#include <zephyr/syscalls/bp_port_write_mrsh.c>

int z_impl_bp_uart_open(uint32_t port, uint32_t baud, uint32_t *generation)
{
	return hub_uart_open(application_port_owner(), port, baud, 0, generation);
}
static inline int z_vrfy_bp_uart_open(uint32_t port, uint32_t baud, uint32_t *generation)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(generation, sizeof(*generation), __alignof__(*generation), true, __func__);
	return z_impl_bp_uart_open(port, baud, generation);
}
#include <zephyr/syscalls/bp_uart_open_mrsh.c>

int z_impl_bp_uart_open_power(uint32_t port, uint32_t baud, uint32_t power_pin, uint32_t *generation)
{
	return hub_uart_open(application_port_owner(), port, baud, power_pin, generation);
}
static inline int z_vrfy_bp_uart_open_power(uint32_t port, uint32_t baud, uint32_t power_pin, uint32_t *generation)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(generation, sizeof(*generation), __alignof__(*generation), true, __func__);
	return z_impl_bp_uart_open_power(port, baud, power_pin, generation);
}
#include <zephyr/syscalls/bp_uart_open_power_mrsh.c>

int z_impl_bp_uart_allow_power(uint32_t allowed)
{
#ifdef CONFIG_BRICKPHYR_PORT_UART
	return hub_uart_allow_power(allowed);
#else
	return -ENOTSUP;
#endif
}
static inline int z_vrfy_bp_uart_allow_power(uint32_t allowed) { return z_impl_bp_uart_allow_power(allowed); }
#include <zephyr/syscalls/bp_uart_allow_power_mrsh.c>

int z_impl_bp_uart_configure(uint32_t port, uint32_t generation, uint32_t baud)
{
	return baud ? hub_uart_configure(application_port_owner(), port, generation, baud) : -EINVAL;
}
static inline int z_vrfy_bp_uart_configure(uint32_t port, uint32_t generation, uint32_t baud)
{
	return z_impl_bp_uart_configure(port, generation, baud);
}
#include <zephyr/syscalls/bp_uart_configure_mrsh.c>

int z_impl_bp_uart_clear(uint32_t port, uint32_t generation)
{
	return hub_uart_configure(application_port_owner(), port, generation, 0);
}
static inline int z_vrfy_bp_uart_clear(uint32_t port, uint32_t generation)
{
	return z_impl_bp_uart_clear(port, generation);
}
#include <zephyr/syscalls/bp_uart_clear_mrsh.c>

int z_impl_bp_uart_waiting(uint32_t port, uint32_t generation)
{
	return hub_uart_read(application_port_owner(), port, generation, NULL, 0);
}
static inline int z_vrfy_bp_uart_waiting(uint32_t port, uint32_t generation)
{
	return z_impl_bp_uart_waiting(port, generation);
}
#include <zephyr/syscalls/bp_uart_waiting_mrsh.c>

int z_impl_bp_uart_read(uint32_t port, uint32_t generation, uint8_t *data, uint32_t size)
{
	return hub_uart_read(application_port_owner(), port, generation, data, size);
}
static inline int z_vrfy_bp_uart_read(uint32_t port, uint32_t generation, uint8_t *data, uint32_t size)
{
	if (!size || size > 128) return size ? -EINVAL : 0;
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(data, size, 1, true, __func__);
	uint8_t copy[128];
	int count = z_impl_bp_uart_read(port, generation, copy, size);
	if (count > 0) memcpy(data, copy, count);
	return count;
}
#include <zephyr/syscalls/bp_uart_read_mrsh.c>

int z_impl_bp_uart_write(uint32_t port, uint32_t generation,
	const uint8_t *data, uint32_t size, uint32_t *request)
{
	return hub_uart_write(application_port_owner(), port, generation, data, size, request);
}
static inline int z_vrfy_bp_uart_write(uint32_t port, uint32_t generation,
	const uint8_t *data, uint32_t size, uint32_t *request)
{
	if (!size || size > 32) return -EINVAL;
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(data, size, 1, false, __func__);
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	uint8_t copy[32];
	memcpy(copy, data, size);
	return z_impl_bp_uart_write(port, generation, copy, size, request);
}
#include <zephyr/syscalls/bp_uart_write_mrsh.c>

int z_impl_bp_port_reset(uint32_t port, uint32_t generation, uint32_t *request)
{
	return hub_port_reset(application_port_owner(), port, generation, request);
}
static inline int z_vrfy_bp_port_reset(uint32_t port, uint32_t generation, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	return z_impl_bp_port_reset(port, generation, request);
}
#include <zephyr/syscalls/bp_port_reset_mrsh.c>

int z_impl_bp_sensor_get(uint32_t port, struct bp_sensor_sample *value)
{
	return hub_sensor_get(port, value);
}
static inline int z_vrfy_bp_sensor_get(uint32_t port, struct bp_sensor_sample *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_sensor_get(port, value);
}
#include <zephyr/syscalls/bp_sensor_get_mrsh.c>

int z_impl_bp_sensor_configure(uint32_t port, uint32_t measurement)
{
	return hub_sensor_configure(application_port_owner(), port, measurement);
}
static inline int z_vrfy_bp_sensor_configure(uint32_t port, uint32_t measurement)
{
	return z_impl_bp_sensor_configure(port, measurement);
}
#include <zephyr/syscalls/bp_sensor_configure_mrsh.c>

int z_impl_bp_sensor_select(uint32_t port, uint32_t generation, uint32_t measurement)
{
	return hub_sensor_select(application_port_owner(), port, generation, measurement);
}
static inline int z_vrfy_bp_sensor_select(uint32_t port, uint32_t generation, uint32_t measurement)
{
	return z_impl_bp_sensor_select(port, generation, measurement);
}
#include <zephyr/syscalls/bp_sensor_select_mrsh.c>

int z_impl_bp_sensor_palette_get(uint32_t port, uint32_t generation, struct bp_sensor_palette *palette)
{
	return hub_sensor_palette_get(port, generation, palette);
}
static inline int z_vrfy_bp_sensor_palette_get(uint32_t port, uint32_t generation, struct bp_sensor_palette *palette)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(palette, sizeof(*palette), __alignof__(*palette), true, __func__);
	return z_impl_bp_sensor_palette_get(port, generation, palette);
}
#include <zephyr/syscalls/bp_sensor_palette_get_mrsh.c>

int z_impl_bp_sensor_palette_set(uint32_t port, uint32_t generation, const struct bp_sensor_palette *palette)
{
	return hub_sensor_palette_set(application_port_owner(), port, generation, palette);
}
static inline int z_vrfy_bp_sensor_palette_set(uint32_t port, uint32_t generation, const struct bp_sensor_palette *palette)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	struct bp_sensor_palette copy;
	if (palette) {
		user_memory_check(palette, sizeof(*palette), 1, false, __func__);
		memcpy(&copy, palette, sizeof(copy));
	}
	return z_impl_bp_sensor_palette_set(port, generation, palette ? &copy : NULL);
}
#include <zephyr/syscalls/bp_sensor_palette_set_mrsh.c>

int z_impl_bp_sensor_light_write(uint32_t port, uint32_t generation,
	const struct bp_sensor_light *light, uint32_t *request)
{
	return hub_sensor_light_write(application_port_owner(), port, generation, light, request);
}
static inline int z_vrfy_bp_sensor_light_write(uint32_t port, uint32_t generation,
	const struct bp_sensor_light *light, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(light, sizeof(*light), 1, false, __func__);
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	struct bp_sensor_light copy;
	memcpy(&copy, light, sizeof(copy));
	return z_impl_bp_sensor_light_write(port, generation, &copy, request);
}
#include <zephyr/syscalls/bp_sensor_light_write_mrsh.c>

int z_impl_bp_port_light_write(uint32_t port, uint32_t generation, uint32_t brightness, uint32_t *request)
{
	return hub_port_light_write(application_port_owner(), port, generation, brightness, request);
}
static inline int z_vrfy_bp_port_light_write(uint32_t port, uint32_t generation, uint32_t brightness, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTORS)) return -ENOTSUP;
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	return z_impl_bp_port_light_write(port, generation, brightness, request);
}
#include <zephyr/syscalls/bp_port_light_write_mrsh.c>

int z_impl_bp_port_matrix_write(uint32_t port, uint32_t generation, const uint32_t *colors, uint32_t *request)
{
	return hub_port_matrix_write(application_port_owner(), port, generation, colors, request);
}
static inline int z_vrfy_bp_port_matrix_write(uint32_t port, uint32_t generation, const uint32_t *colors, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	uint32_t copy[9];
	if (colors) {
		user_memory_check(colors, sizeof(copy), 1, false, __func__);
		memcpy(copy, colors, sizeof(copy));
	}
	return z_impl_bp_port_matrix_write(port, generation, colors ? copy : NULL, request);
}
#include <zephyr/syscalls/bp_port_matrix_write_mrsh.c>

int z_impl_bp_port_pf_write(uint32_t port, uint32_t generation,
	const struct bp_pf_command *command, uint32_t *request)
{
	return hub_port_pf_write(application_port_owner(), port, generation, command, request);
}
static inline int z_vrfy_bp_port_pf_write(uint32_t port, uint32_t generation,
	const struct bp_pf_command *command, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART)) return -ENOTSUP;
	user_memory_check(command, sizeof(*command), 1, false, __func__);
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	struct bp_pf_command copy;
	memcpy(&copy, command, sizeof(copy));
	return z_impl_bp_port_pf_write(port, generation, &copy, request);
}
#include <zephyr/syscalls/bp_port_pf_write_mrsh.c>

int z_impl_bp_sensor_release(uint32_t port)
{
	return hub_port_release(application_port_owner(), port);
}
static inline int z_vrfy_bp_sensor_release(uint32_t port)
{
	return z_impl_bp_sensor_release(port);
}
#include <zephyr/syscalls/bp_sensor_release_mrsh.c>

int z_impl_bp_port_release(uint32_t port) { return hub_port_release(application_port_owner(), port); }
static inline int z_vrfy_bp_port_release(uint32_t port) { return z_impl_bp_port_release(port); }
#include <zephyr/syscalls/bp_port_release_mrsh.c>

int z_impl_bp_motor_drive(uint32_t port, int32_t duty, uint32_t duration_ms)
{
	return hub_motor_drive(application_port_owner(), port, duty, duration_ms);
}
static inline int z_vrfy_bp_motor_drive(uint32_t port, int32_t duty, uint32_t duration_ms)
{
	return z_impl_bp_motor_drive(port, duty, duration_ms);
}
#include <zephyr/syscalls/bp_motor_drive_mrsh.c>

int z_impl_bp_motor_coast(uint32_t port) { return hub_motor_coast(application_port_owner(), port); }
static inline int z_vrfy_bp_motor_coast(uint32_t port) { return z_impl_bp_motor_coast(port); }
#include <zephyr/syscalls/bp_motor_coast_mrsh.c>

int z_impl_bp_motor_get(uint32_t port, struct bp_motor_state *value) { return hub_motor_get(port, value); }
static inline int z_vrfy_bp_motor_get(uint32_t port, struct bp_motor_state *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTORS)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_motor_get(port, value);
}
#include <zephyr/syscalls/bp_motor_get_mrsh.c>

int z_impl_bp_motor_setup(uint32_t port, uint32_t generation, int32_t direction, uint32_t *request)
{
	return hub_motor_setup(application_port_owner(), port, generation, direction, request);
}
static inline int z_vrfy_bp_motor_setup(uint32_t port, uint32_t generation, int32_t direction, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTORS)) return -ENOTSUP;
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	return z_impl_bp_motor_setup(port, generation, direction, request);
}
#include <zephyr/syscalls/bp_motor_setup_mrsh.c>

int z_impl_bp_motor_dc(uint32_t port, uint32_t generation, int32_t duty, uint32_t coast, uint32_t *request)
{
	return hub_motor_dc(application_port_owner(), port, generation, duty, coast, request);
}
static inline int z_vrfy_bp_motor_dc(uint32_t port, uint32_t generation, int32_t duty, uint32_t coast, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTORS)) return -ENOTSUP;
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	return z_impl_bp_motor_dc(port, generation, duty, coast, request);
}
#include <zephyr/syscalls/bp_motor_dc_mrsh.c>

int z_impl_bp_motor_set_voltage(uint32_t port, uint32_t generation, int32_t max_voltage)
{
	return hub_motor_set_voltage(application_port_owner(), port, generation, max_voltage);
}
static inline int z_vrfy_bp_motor_set_voltage(uint32_t port, uint32_t generation, int32_t max_voltage)
{
	return z_impl_bp_motor_set_voltage(port, generation, max_voltage);
}
#include <zephyr/syscalls/bp_motor_set_voltage_mrsh.c>

int z_impl_bp_motor_close(uint32_t port, uint32_t generation)
{
	return hub_motor_close(application_port_owner(), port, generation);
}
static inline int z_vrfy_bp_motor_close(uint32_t port, uint32_t generation)
{
	return z_impl_bp_motor_close(port, generation);
}
#include <zephyr/syscalls/bp_motor_close_mrsh.c>

int z_impl_bp_motor_observe(uint32_t port, uint32_t generation, uint32_t *request)
{
	return hub_motor_observe(application_port_owner(), port, generation, request);
}
static inline int z_vrfy_bp_motor_observe(uint32_t port, uint32_t generation, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	return z_impl_bp_motor_observe(port, generation, request);
}
#include <zephyr/syscalls/bp_motor_observe_mrsh.c>

int z_impl_bp_motor_observation_get(uint32_t port, uint32_t generation, uint32_t window_ms,
	struct bp_motor_observation *value)
{
	return hub_motor_observation_get(port, generation, window_ms, value);
}
static inline int z_vrfy_bp_motor_observation_get(uint32_t port, uint32_t generation, uint32_t window_ms,
	struct bp_motor_observation *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_motor_observation_get(port, generation, window_ms, value);
}
#include <zephyr/syscalls/bp_motor_observation_get_mrsh.c>

int z_impl_bp_control_frame_get(uint32_t port_mask, struct bp_control_frame *frame)
{
	return control_frame_get(port_mask, frame);
}
static inline int z_vrfy_bp_control_frame_get(uint32_t port_mask, struct bp_control_frame *frame)
{
	if (port_mask >> BP_PORT_COUNT) return -EINVAL;
	user_memory_check(frame, sizeof(*frame), __alignof__(*frame), true, __func__);
	return z_impl_bp_control_frame_get(port_mask, frame);
}
#include <zephyr/syscalls/bp_control_frame_get_mrsh.c>

int z_impl_bp_control_wait(uint32_t period_us, struct bp_control_step *step,
	void *parameters, size_t capacity)
{
	if (!native_caller()) return -EPERM;
	int64_t ticks = k_us_to_ticks_ceil64(period_us);
	if (!period_us || k_ticks_to_us_ceil64(ticks) != period_us) return -EINVAL;
	if (control_clock.period_us && control_clock.period_us != period_us) return -EINVAL;
	size_t size = 0;
#ifdef CONFIG_BRICKPHYR_PYTHON
	if (controller) size = controller->parameter_size;
#endif
	if (capacity < size) return -ENOBUFS;
	int64_t now = k_uptime_ticks();
	int64_t due = control_clock.period_us ? control_clock.next_tick : now;
	if (due > now) k_sleep(K_TIMEOUT_ABS_TICKS(due));
	now = k_uptime_ticks();
	uint32_t skipped = 0;
	if (now > due) {
		int64_t missed = (now - due) / ticks;
		skipped = MIN(missed, UINT32_MAX);
		due += missed * ticks;
	}
	struct bp_control_step value = {
		.scheduled_us = k_ticks_to_us_floor64(due), .started_us = control_time_us(),
		.sequence = control_clock.step.sequence + 1, .skipped = skipped, .parameter_size = size,
	};
	k_spinlock_key_t key = k_spin_lock(&control_lock);
#ifdef CONFIG_BRICKPHYR_PYTHON
	if (controller) {
		if (size) memcpy(parameters, controller->parameters, size);
		value.parameter_revision = controller->revision;
	}
#endif
	control_clock.period_us = period_us;
	control_clock.next_tick = due + ticks;
	control_clock.step = value;
	k_spin_unlock(&control_lock, key);
	*step = value;
	return 0;
}
static inline int z_vrfy_bp_control_wait(uint32_t period_us, struct bp_control_step *step,
	void *parameters, size_t capacity)
{
	user_memory_check(step, sizeof(*step), __alignof__(*step), true, __func__);
	if (capacity) user_memory_check(parameters, capacity, 1, true, __func__);
	return z_impl_bp_control_wait(period_us, step, parameters, capacity);
}
#include <zephyr/syscalls/bp_control_wait_mrsh.c>

#ifdef CONFIG_BRICKPHYR_PYTHON
int z_impl_application_control_start(const void *image, size_t size,
	const void *parameters, size_t length)
{
	if (controller || atomic_get(&control_pending)) return -EBUSY;
	control_request.image = image;
	control_request.size = size;
	control_request.parameters = parameters;
	control_request.length = length;
	atomic_set(&control_pending, 1);
	k_sem_take(&control_done, K_FOREVER);
	return control_request.result;
}
static inline int z_vrfy_application_control_start(const void *image, size_t size,
	const void *parameters, size_t length)
{
	verify_python_thread();
	if (!size || size > APPLICATION_IMAGE_LIMIT || length > APPLICATION_CONTROL_PARAMETERS_MAX) return -EINVAL;
	user_memory_check(image, size, 1, false, __func__);
	if (length) user_memory_check(parameters, length, 1, false, __func__);
	return z_impl_application_control_start(image, size, parameters, length);
}
#include <zephyr/syscalls/application_control_start_mrsh.c>

int z_impl_application_control_update(const void *parameters, size_t length)
{
	if (!controller || !controller->started) return -ENODATA;
	if (length != controller->parameter_size) return -EMSGSIZE;
	k_spinlock_key_t key = k_spin_lock(&control_lock);
	int result = -EOVERFLOW;
	if (controller->revision < INT32_MAX) {
		if (length) memcpy(controller->parameters, parameters, length);
		result = ++controller->revision;
	}
	k_spin_unlock(&control_lock, key);
	return result;
}
static inline int z_vrfy_application_control_update(const void *parameters, size_t length)
{
	verify_python_thread();
	if (length > APPLICATION_CONTROL_PARAMETERS_MAX) return -EINVAL;
	if (length) user_memory_check(parameters, length, 1, false, __func__);
	return z_impl_application_control_update(parameters, length);
}
#include <zephyr/syscalls/application_control_update_mrsh.c>

int z_impl_application_control_get(struct application_control_status *value)
{
	if (!controller || !controller->started) return -ENODATA;
	k_spinlock_key_t key = k_spin_lock(&control_lock);
	value->step = control_clock.step;
	value->revision = controller->revision;
	value->period_us = control_clock.period_us;
	value->stack_bytes = K_THREAD_STACK_LEN(controller->stack_size);
	value->loader_bytes = controller->loader_size;
	value->loaded_bytes = extension->alloc_size;
	k_spin_unlock(&control_lock, key);
	return 0;
}
static inline int z_vrfy_application_control_get(struct application_control_status *value)
{
	verify_python_thread();
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_application_control_get(value);
}
#include <zephyr/syscalls/application_control_get_mrsh.c>
#endif

int z_impl_bp_motor_group_set(const struct bp_motor_group_command *command, uint32_t *request)
{
	return hub_motor_group_set(application_port_owner(), command, request);
}
static inline int z_vrfy_bp_motor_group_set(const struct bp_motor_group_command *command, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTORS)) return -ENOTSUP;
	user_memory_check(command, sizeof(*command), __alignof__(*command), false, __func__);
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	struct bp_motor_group_command copy = *command;
	return z_impl_bp_motor_group_set(&copy, request);
}
#include <zephyr/syscalls/bp_motor_group_set_mrsh.c>

int z_impl_bp_motor_group_get(struct bp_motor_group_state *state)
{
	return hub_motor_group_get(state);
}
static inline int z_vrfy_bp_motor_group_get(struct bp_motor_group_state *state)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTORS)) return -ENOTSUP;
	user_memory_check(state, sizeof(*state), __alignof__(*state), true, __func__);
	return z_impl_bp_motor_group_get(state);
}
#include <zephyr/syscalls/bp_motor_group_get_mrsh.c>

int z_impl_bp_record_begin(const struct bp_record_spec *spec)
{
	/* Allocation belongs to the supervisor. Aborting this caller must not
	 * strand a just-allocated buffer or a lock in the application's thread. */
	record_request = *spec;
	atomic_set(&record_pending, 1);
	k_sem_take(&record_done, K_FOREVER);
	return record_result;
}
static inline int z_vrfy_bp_record_begin(const struct bp_record_spec *spec)
{
	K_OOPS(K_SYSCALL_VERIFY(native_caller()));
	user_memory_check(spec, sizeof(*spec), __alignof__(*spec), false, __func__);
	return z_impl_bp_record_begin(spec);
}
#include <zephyr/syscalls/bp_record_begin_mrsh.c>

int z_impl_bp_record_write(const void *data, size_t length)
{
	return application_record_write(data, length);
}
static inline int z_vrfy_bp_record_write(const void *data, size_t length)
{
	if (length > 1024) return -EMSGSIZE;
	if (length) user_memory_check(data, length, 1, false, __func__);
	return z_impl_bp_record_write(data, length);
}
#include <zephyr/syscalls/bp_record_write_mrsh.c>

int z_impl_bp_motor_control(uint32_t port, const struct bp_motor_command *command)
{
	return hub_motor_control(application_port_owner(), port, command);
}
static inline int z_vrfy_bp_motor_control(uint32_t port, const struct bp_motor_command *command)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(command, sizeof(*command), __alignof__(*command), false, __func__);
	/* Copy user storage before taking any hardware service lock. */
	struct bp_motor_command copy = *command;
	return z_impl_bp_motor_control(port, &copy);
}
#include <zephyr/syscalls/bp_motor_control_mrsh.c>

int z_impl_bp_servo_setup(uint32_t port, uint32_t generation,
	const struct bp_servo_setup *setup, uint32_t *request)
{
	return hub_servo_setup(application_port_owner(), port, generation, setup, request);
}
static inline int z_vrfy_bp_servo_setup(uint32_t port, uint32_t generation,
	const struct bp_servo_setup *setup, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(setup, sizeof(*setup), 1, false, __func__);
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	struct bp_servo_setup copy;
	memcpy(&copy, setup, sizeof(copy));
	return z_impl_bp_servo_setup(port, generation, &copy, request);
}
#include <zephyr/syscalls/bp_servo_setup_mrsh.c>

int z_impl_bp_servo_get(uint32_t port, uint32_t generation, uint32_t window_ms, struct bp_servo_state *value)
{
	return hub_servo_get(port, generation, window_ms, value);
}
static inline int z_vrfy_bp_servo_get(uint32_t port, uint32_t generation, uint32_t window_ms, struct bp_servo_state *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_servo_get(port, generation, window_ms, value);
}
#include <zephyr/syscalls/bp_servo_get_mrsh.c>

int z_impl_bp_servo_reset(uint32_t port, uint32_t generation, int32_t angle, uint32_t absolute, uint32_t *request)
{
	return hub_servo_reset(application_port_owner(), port, generation, angle, absolute, request);
}
static inline int z_vrfy_bp_servo_reset(uint32_t port, uint32_t generation, int32_t angle, uint32_t absolute, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	return z_impl_bp_servo_reset(port, generation, angle, absolute, request);
}
#include <zephyr/syscalls/bp_servo_reset_mrsh.c>

int z_impl_bp_servo_move(uint32_t port, uint32_t generation,
	const struct bp_servo_command *command, uint32_t *request)
{
	return hub_servo_move(application_port_owner(), port, generation, command, request);
}
static inline int z_vrfy_bp_servo_move(uint32_t port, uint32_t generation,
	const struct bp_servo_command *command, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(command, sizeof(*command), 1, false, __func__);
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	struct bp_servo_command copy;
	memcpy(&copy, command, sizeof(copy));
	return z_impl_bp_servo_move(port, generation, &copy, request);
}
#include <zephyr/syscalls/bp_servo_move_mrsh.c>

int z_impl_bp_servo_settings_get(uint32_t port, uint32_t generation, struct bp_servo_settings *value)
{
	return hub_servo_settings_get(port, generation, value);
}
static inline int z_vrfy_bp_servo_settings_get(uint32_t port, uint32_t generation, struct bp_servo_settings *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_servo_settings_get(port, generation, value);
}
#include <zephyr/syscalls/bp_servo_settings_get_mrsh.c>

int z_impl_bp_servo_trajectory_get(uint32_t port, uint32_t generation, struct bp_servo_trajectory *value)
{
	return hub_servo_trajectory_get(port, generation, value);
}
static inline int z_vrfy_bp_servo_trajectory_get(uint32_t port, uint32_t generation, struct bp_servo_trajectory *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_servo_trajectory_get(port, generation, value);
}
#include <zephyr/syscalls/bp_servo_trajectory_get_mrsh.c>

int z_impl_bp_servo_model_get(uint32_t port, uint32_t generation, struct bp_motor_model_state *value)
{
	return hub_servo_model_get(port, generation, value);
}
static inline int z_vrfy_bp_servo_model_get(uint32_t port, uint32_t generation, struct bp_motor_model_state *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_servo_model_get(port, generation, value);
}
#include <zephyr/syscalls/bp_servo_model_get_mrsh.c>

int z_impl_bp_motor_log_status(uint32_t port, uint32_t generation, uint32_t kind, uint32_t stop, struct bp_motor_log_status *value)
{
	return hub_motor_log_status(application_port_owner(), port, generation, kind, stop, value);
}
static inline int z_vrfy_bp_motor_log_status(uint32_t port, uint32_t generation, uint32_t kind, uint32_t stop, struct bp_motor_log_status *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_motor_log_status(port, generation, kind, stop, value);
}
#include <zephyr/syscalls/bp_motor_log_status_mrsh.c>

int z_impl_bp_motor_log_read(uint32_t port, uint32_t generation, uint32_t kind, uint32_t revision, uint32_t row, struct bp_motor_log_row *value)
{
	return hub_motor_log_read(application_port_owner(), port, generation, kind, revision, row, value);
}
static inline int z_vrfy_bp_motor_log_read(uint32_t port, uint32_t generation, uint32_t kind, uint32_t revision, uint32_t row, struct bp_motor_log_row *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_motor_log_read(port, generation, kind, revision, row, value);
}
#include <zephyr/syscalls/bp_motor_log_read_mrsh.c>

int z_impl_bp_motor_log_start(uint32_t port, uint32_t generation, uint32_t kind, uint32_t duration_ms, uint32_t down_sample)
{
	return hub_motor_log_start(application_port_owner(), port, generation, kind, duration_ms, down_sample);
}
static inline int z_vrfy_bp_motor_log_start(uint32_t port, uint32_t generation, uint32_t kind, uint32_t duration_ms, uint32_t down_sample)
{
	return z_impl_bp_motor_log_start(port, generation, kind, duration_ms, down_sample);
}
#include <zephyr/syscalls/bp_motor_log_start_mrsh.c>

int z_impl_bp_servo_settings_set(uint32_t port, uint32_t generation, uint32_t group,
	const struct bp_servo_settings *value, uint32_t *revision)
{
	return hub_servo_settings_set(application_port_owner(), port, generation, group, value, revision);
}
static inline int z_vrfy_bp_servo_settings_set(uint32_t port, uint32_t generation, uint32_t group,
	const struct bp_servo_settings *value, uint32_t *revision)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), 1, false, __func__);
	user_memory_check(revision, sizeof(*revision), __alignof__(*revision), true, __func__);
	struct bp_servo_settings copy;
	memcpy(&copy, value, sizeof(copy));
	return z_impl_bp_servo_settings_set(port, generation, group, &copy, revision);
}
#include <zephyr/syscalls/bp_servo_settings_set_mrsh.c>

int z_impl_bp_drivebase_open(const struct bp_drivebase_setup *setup, uint32_t *handle)
{
	return hub_drivebase_open(application_port_owner(), setup, handle);
}
static inline int z_vrfy_bp_drivebase_open(const struct bp_drivebase_setup *setup, uint32_t *handle)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(setup, sizeof(*setup), 1, false, __func__);
	user_memory_check(handle, sizeof(*handle), __alignof__(*handle), true, __func__);
	struct bp_drivebase_setup copy;
	memcpy(&copy, setup, sizeof(copy));
	return z_impl_bp_drivebase_open(&copy, handle);
}
#include <zephyr/syscalls/bp_drivebase_open_mrsh.c>

int z_impl_bp_drivebase_get(uint32_t handle, struct bp_drivebase_state *value)
{
	return hub_drivebase_get(application_port_owner(), handle, value);
}
static inline int z_vrfy_bp_drivebase_get(uint32_t handle, struct bp_drivebase_state *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_drivebase_get(handle, value);
}
#include <zephyr/syscalls/bp_drivebase_get_mrsh.c>

int z_impl_bp_drivebase_command(uint32_t handle, const struct bp_drivebase_command *command, uint32_t *request)
{
	return hub_drivebase_command(application_port_owner(), handle, command, request);
}
static inline int z_vrfy_bp_drivebase_command(uint32_t handle, const struct bp_drivebase_command *command, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(command, sizeof(*command), 1, false, __func__);
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	struct bp_drivebase_command copy;
	memcpy(&copy, command, sizeof(copy));
	return z_impl_bp_drivebase_command(handle, &copy, request);
}
#include <zephyr/syscalls/bp_drivebase_command_mrsh.c>

int z_impl_bp_drivebase_settings_set(uint32_t handle, const struct bp_drivebase_settings *settings, uint32_t *request)
{
	return hub_drivebase_settings_set(application_port_owner(), handle, settings, request);
}
static inline int z_vrfy_bp_drivebase_settings_set(uint32_t handle, const struct bp_drivebase_settings *settings, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(settings, sizeof(*settings), 1, false, __func__);
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	struct bp_drivebase_settings copy;
	memcpy(&copy, settings, sizeof(copy));
	return z_impl_bp_drivebase_settings_set(handle, &copy, request);
}
#include <zephyr/syscalls/bp_drivebase_settings_set_mrsh.c>

int z_impl_bp_drivebase_close(uint32_t handle)
{
	return hub_drivebase_close(application_port_owner(), handle);
}
static inline int z_vrfy_bp_drivebase_close(uint32_t handle)
{
	return z_impl_bp_drivebase_close(handle);
}
#include <zephyr/syscalls/bp_drivebase_close_mrsh.c>

int z_impl_bp_drivebase_axis_get(uint32_t handle, uint32_t axis, struct bp_drivebase_axis_state *value)
{
	return hub_drivebase_axis_get(application_port_owner(), handle, axis, value);
}
static inline int z_vrfy_bp_drivebase_axis_get(uint32_t handle, uint32_t axis, struct bp_drivebase_axis_state *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_drivebase_axis_get(handle, axis, value);
}
#include <zephyr/syscalls/bp_drivebase_axis_get_mrsh.c>

int z_impl_bp_drivebase_axis_set(uint32_t handle, uint32_t axis, uint32_t group,
	const struct bp_servo_settings *value, uint32_t *request)
{
	return hub_drivebase_axis_set(application_port_owner(), handle, axis, group, value, request);
}
static inline int z_vrfy_bp_drivebase_axis_set(uint32_t handle, uint32_t axis, uint32_t group,
	const struct bp_servo_settings *value, uint32_t *request)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), 1, false, __func__);
	user_memory_check(request, sizeof(*request), __alignof__(*request), true, __func__);
	struct bp_servo_settings copy;
	memcpy(&copy, value, sizeof(copy));
	return z_impl_bp_drivebase_axis_set(handle, axis, group, &copy, request);
}
#include <zephyr/syscalls/bp_drivebase_axis_set_mrsh.c>

int z_impl_bp_drivebase_log_start(uint32_t handle, uint32_t axis, uint32_t duration_ms, uint32_t down_sample)
{
	return hub_drivebase_log_start(application_port_owner(), handle, axis, duration_ms, down_sample);
}
static inline int z_vrfy_bp_drivebase_log_start(uint32_t handle, uint32_t axis, uint32_t duration_ms, uint32_t down_sample)
{
	return z_impl_bp_drivebase_log_start(handle, axis, duration_ms, down_sample);
}
#include <zephyr/syscalls/bp_drivebase_log_start_mrsh.c>

int z_impl_bp_drivebase_log_status(uint32_t handle, uint32_t axis, uint32_t stop, struct bp_motor_log_status *value)
{
	return hub_drivebase_log_status(application_port_owner(), handle, axis, stop, value);
}
static inline int z_vrfy_bp_drivebase_log_status(uint32_t handle, uint32_t axis, uint32_t stop, struct bp_motor_log_status *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_drivebase_log_status(handle, axis, stop, value);
}
#include <zephyr/syscalls/bp_drivebase_log_status_mrsh.c>

int z_impl_bp_drivebase_log_read(uint32_t handle, uint32_t axis, uint32_t revision, uint32_t row,
	struct bp_motor_log_row *value)
{
	return hub_drivebase_log_read(application_port_owner(), handle, axis, revision, row, value);
}
static inline int z_vrfy_bp_drivebase_log_read(uint32_t handle, uint32_t axis, uint32_t revision, uint32_t row,
	struct bp_motor_log_row *value)
{
	if (!IS_ENABLED(CONFIG_BRICKPHYR_MOTOR_CONTROL)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_drivebase_log_read(handle, axis, revision, row, value);
}
#include <zephyr/syscalls/bp_drivebase_log_read_mrsh.c>

int z_impl_bp_sound_tone(uint32_t frequency, uint32_t duration_ms, uint32_t volume)
{
	return hub_sound_tone(BP_SOUND_APPLICATION, frequency, duration_ms, volume);
}
static inline int z_vrfy_bp_sound_tone(uint32_t frequency, uint32_t duration_ms, uint32_t volume)
{
	return z_impl_bp_sound_tone(frequency, duration_ms, volume);
}
#include <zephyr/syscalls/bp_sound_tone_mrsh.c>

int z_impl_bp_sound_reserve(void)
{
	return hub_sound_reserve(BP_SOUND_APPLICATION);
}
static inline int z_vrfy_bp_sound_reserve(void) { return z_impl_bp_sound_reserve(); }
#include <zephyr/syscalls/bp_sound_reserve_mrsh.c>

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
	if (!IS_ENABLED(CONFIG_BRICKPHYR_SOUND)) return -ENOTSUP;
	user_memory_check(samples, count * sizeof(*samples), __alignof__(*samples), false, __func__);
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
	if (!IS_ENABLED(CONFIG_BRICKPHYR_SOUND)) return -ENOTSUP;
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_sound_get(value);
}
#include <zephyr/syscalls/bp_sound_get_mrsh.c>

int z_impl_bp_system_get(struct bp_system_info *value)
{
	/* Launch metadata is immutable while this application's thread runs. */
	*value = (struct bp_system_info){.program_id = status.program_id, .program_start_type = 3};
	strncpy(value->name, hub_name(), sizeof(value->name) - 1);
	value->reset_error = hub_reset_cause(&value->reset_cause);
	struct hub_power_state power;
	hub_power_get(&power);
	value->connected_usb = power.transport == HUB_USB;
	value->connected_ble = power.transport == HUB_BLE;
	return 0;
}
static inline int z_vrfy_bp_system_get(struct bp_system_info *value)
{
	user_memory_check(value, sizeof(*value), __alignof__(*value), true, __func__);
	return z_impl_bp_system_get(value);
}
#include <zephyr/syscalls/bp_system_get_mrsh.c>

int z_impl_bp_set_stop_buttons(uint32_t buttons)
{
	if (buttons & ~15U) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&button_lock);
	stop_buttons = buttons;
	stop_pressed = buttons && (hub_buttons() & buttons) == buttons;
	k_spin_unlock(&button_lock, key);
	return 0;
}
static inline int z_vrfy_bp_set_stop_buttons(uint32_t buttons) { return z_impl_bp_set_stop_buttons(buttons); }
#include <zephyr/syscalls/bp_set_stop_buttons_mrsh.c>

uint32_t z_impl_bp_get_stop_buttons(void)
{
	k_spinlock_key_t key = k_spin_lock(&button_lock);
	uint32_t buttons = stop_buttons;
	k_spin_unlock(&button_lock, key);
	return buttons;
}
static inline uint32_t z_vrfy_bp_get_stop_buttons(void) { return z_impl_bp_get_stop_buttons(); }
#include <zephyr/syscalls/bp_get_stop_buttons_mrsh.c>

void z_impl_bp_shutdown(void)
{
	hub_power_request_shutdown();
	for (;;) k_sleep(K_FOREVER);
}
static inline void z_vrfy_bp_shutdown(void) { z_impl_bp_shutdown(); }
#include <zephyr/syscalls/bp_shutdown_mrsh.c>

int z_impl_bp_user_data_read(uint32_t offset, void *data, size_t size)
{
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
	return app_data_read(offset, data, size);
#else
	return -ENOTSUP;
#endif
}
static inline int z_vrfy_bp_user_data_read(uint32_t offset, void *data, size_t size)
{
	if (offset > BP_USER_DATA_SIZE || size > BP_USER_DATA_SIZE - offset) return -EINVAL;
	if (!IS_ENABLED(CONFIG_BRICKPHYR_APP_STORAGE)) return -ENOTSUP;
	if (size) user_memory_check(data, size, 1, true, __func__);
	return z_impl_bp_user_data_read(offset, data, size);
}
#include <zephyr/syscalls/bp_user_data_read_mrsh.c>

int z_impl_bp_user_data_write(uint32_t offset, const void *data, size_t size)
{
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
	return app_data_write(offset, data, size);
#else
	return -ENOTSUP;
#endif
}
static inline int z_vrfy_bp_user_data_write(uint32_t offset, const void *data, size_t size)
{
	if (offset > BP_USER_DATA_SIZE || size > BP_USER_DATA_SIZE - offset) return -EINVAL;
	if (!IS_ENABLED(CONFIG_BRICKPHYR_APP_STORAGE)) return -ENOTSUP;
	if (size) user_memory_check(data, size, 1, false, __func__);
	return z_impl_bp_user_data_write(offset, data, size);
}
#include <zephyr/syscalls/bp_user_data_write_mrsh.c>

/* The first context to complete owns the result, even if cleanup preempts the
 * other context just as it returns. All memory stays mapped until both join. */
static void finish_application(int result, unsigned int fault)
{
	k_spinlock_key_t key = k_spin_lock(&control_lock);
	if (!finished_source) {
		finished_source = k_sched_current_thread_query() == &application_thread ? 1 : 2;
		atomic_set(&return_value, result);
		atomic_set(&fault_reason, fault);
	}
	k_spin_unlock(&control_lock, key);
}

void z_impl_application_finished(int result) { finish_application(result, 0); }

static inline void z_vrfy_application_finished(int result)
{
	K_OOPS(K_SYSCALL_VERIFY(k_sched_current_thread_query() == &application_thread || native_caller()));
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
		python_stack_bottom);
	application_finished(result);
}
#endif

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	/* A fault may precede initialization of the thread's TLS cache. */
	if (atomic_get(&active) && (k_sched_current_thread_query() == &application_thread || native_caller()) &&
	    reason != K_ERR_KERNEL_PANIC && esf && !(esf->basic.xpsr & 0x1ffU)) {
		finish_application(0, reason + 1);
		return; /* Zephyr aborts this thread; the supervisor joins before unload. */
	}
	k_fatal_halt(reason);
}

static int clear_domain(struct k_mem_domain *domain)
{
	for (size_t i = 0; i < ARRAY_SIZE(domain->partitions); i++) {
		struct k_mem_partition part = domain->partitions[i];
		if (part.size) {
			int err = k_mem_domain_remove_partition(domain, &part);
			if (err) return err;
		}
	}
	return 0;
}

/* Domain mappings and live threads must be gone before releasing native code. */
static int release_native(void)
{
	int err = extension ? llext_unload(&extension) : 0;
	if (!err && loader_initialized) {
		err = llext_heap_uninit();
		if (!err) loader_initialized = false;
	}
	if (!err) {
		k_free(native_memory);
		native_memory = NULL;
	}
	return err;
}

#ifdef CONFIG_BRICKPHYR_PYTHON
static int release_controller(void)
{
	if (!controller) return 0;
	int err = controller->domain_initialized ? clear_domain(&controller->domain) : 0;
	if (!err) err = release_native();
	if (!err && controller->stack) {
		err = k_thread_stack_free(controller->stack);
		if (!err) controller->stack = NULL;
	}
	if (!err) {
		if (controller->thread) k_object_free(controller->thread);
		k_free(controller);
		controller = NULL;
	}
	return err;
}
#endif

static int release(void)
{
	/* Caller has joined the application. Retain its bounded record tail even
	 * after a fault; cancellation prevents allocation from an abandoned begin. */
	atomic_clear(&record_pending);
	k_sem_reset(&record_done);
	application_record_end();
	int err = hub_ports_release(BP_PORT_APPLICATION);
#ifdef CONFIG_BRICKPHYR_PYTHON
	int controller_error = hub_ports_release(BP_PORT_CONTROLLER);
	if (!err) err = controller_error;
	atomic_clear(&control_pending);
	k_sem_reset(&control_done);
#endif
	int radio_error = hub_radio_release();
	if (!err) err = radio_error;
	int peer_error = hub_ble_peer_release();
	if (!err) err = peer_error;
	int network_error = hub_network_release();
	if (!err) err = network_error;
	hub_imu_release();
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
		err = clear_domain(&application_domain);
	}
#ifdef CONFIG_BRICKPHYR_PYTHON
	if (!err) err = release_controller();
#endif
	if (!err) err = release_native();
#ifdef CONFIG_BRICKPHYR_PYTHON
	/* A queued worker acquires lifecycle before reading the request. Joining
	 * its caller and clearing pending here also cancels an unstarted copy. */
	atomic_clear(&code_pending);
	/* The caller has joined the application; no new lease can race cleanup. */
	z_impl_application_message_close(message.lease);
	k_sem_reset(&code_done);
	if (!err) {
		k_free(code_memory);
		code_memory = NULL;
		code_used = code_reserved = 0;
	}
#endif
	if (!err && application_stack) {
		err = k_thread_stack_free(application_stack);
		if (!err) {
			application_stack = NULL;
			application_stack_size = 0;
		}
	}
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
	bool primary_ended = k_thread_join(&application_thread, K_NO_WAIT) == 0;
	bool ended = primary_ended;
#ifdef CONFIG_BRICKPHYR_PYTHON
	bool controller_live = controller && controller->started;
	bool controller_ended = controller_live && k_thread_join(controller->thread, K_NO_WAIT) == 0;
	ended |= controller_ended;
#endif
	if (!ended && !stop) {
		return -EAGAIN;
	}
	if (!primary_ended) {
		k_thread_abort(&application_thread);
		int err = k_thread_join(&application_thread, K_FOREVER);
		if (err) {
			return err;
		}
	}
#ifdef CONFIG_BRICKPHYR_PYTHON
	if (controller_live && !controller_ended) {
		k_thread_abort(controller->thread);
		int err = k_thread_join(controller->thread, K_FOREVER);
		if (err) return err;
	}
#endif
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
	if (!err && finished_source == 1 && ended && !stop && !fault && status.repl &&
	    result == PYTHON_RESTART && !shutting_down) {
		return run_python(NULL, 0, APPLICATION_PYTHON, true, 128);
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

int application_clear_recording(void)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	(void)reap(false);
	int err = atomic_get(&active) ? -EBUSY : 0;
	if (!err) application_record_clear();
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
#ifdef CONFIG_BRICKPHYR_PYTHON
	if (atomic_get(&control_pending)) {
		control_request.result = start_controller();
		atomic_clear(&control_pending);
		k_sem_give(&control_done);
	}
#endif
	if (atomic_get(&record_pending)) {
		record_result = application_record_begin(&record_request);
		atomic_clear(&record_pending);
		k_sem_give(&record_done);
	}
	expire_upload();
	k_mutex_unlock(&lifecycle);
}

void application_buttons(uint32_t buttons)
{
	if (k_mutex_lock(&lifecycle, K_NO_WAIT)) return;
	k_spinlock_key_t key = k_spin_lock(&button_lock);
#ifdef CONFIG_BRICKPHYR_PYTHON
	bool changed = buttons != observed_buttons;
#endif
	observed_buttons = buttons;
	bool pressed = stop_buttons && (buttons & stop_buttons) == stop_buttons;
	bool trigger = pressed && !stop_pressed;
	stop_pressed = pressed;
	k_spin_unlock(&button_lock, key);
#ifdef CONFIG_BRICKPHYR_PYTHON
	/* Menu waits use the VM event semaphore, including releases and buttons
	 * that do not stop the application. Unchanged polls must not wake the VM. */
	if (changed) k_sem_give(&io_ready);
#endif
	if (trigger && atomic_get(&active)) {
#ifdef CONFIG_BRICKPHYR_PYTHON
		if (status.format != APPLICATION_NATIVE) {
			python_stop();
			k_sem_give(&io_ready);
		} else
#endif
		{
			(void)reap(true);
		}
	}
	k_mutex_unlock(&lifecycle);
}

/* lifecycle is held; neither engine may replace a thread before it is joined. */
static int prepare(size_t stack_size)
{
	if (shutting_down) {
		return -ESHUTDOWN;
	}
	/* Allocation belongs to the calling supervisor, not the application. Restore
	 * its resource pool before any other work or thread creation can inherit it.
	 */
	struct k_thread *caller = k_current_get();
	struct k_heap *pool = caller->resource_pool;
	k_thread_system_pool_assign(caller);
	k_thread_stack_t *stack = k_thread_stack_alloc(stack_size, K_USER);
	k_thread_heap_assign(caller, pool);
	if (!stack) {
		return -ENOMEM;
	}
	int err = reap(true);
	if (err) {
		goto failed;
	}
	if (!hub_power_application(true)) {
		err = -ESHUTDOWN;
		goto failed;
	}
	if (!domain_initialized) {
		err = k_mem_domain_init(&application_domain, 0, NULL);
		domain_initialized = err == 0;
	}
	if (err) {
		hub_power_application(false);
		goto failed;
	}
	application_stack = stack;
	application_stack_size = stack_size;
	return 0;
failed:
	/* This reservation was never attached to a thread. Keep the first error. */
	(void)k_thread_stack_free(stack);
	return err;
}

static int start_thread(k_thread_entry_t entry, void *first, void *second, void *third,
	struct application_status value, bool soft_reset)
{
	status = value;
	status.state = APPLICATION_RUNNING;
	status.stack_bytes = K_THREAD_STACK_LEN(application_stack_size);
	(void)z_impl_bp_set_stop_buttons(value.format == APPLICATION_NATIVE ? 0 : 1);
	atomic_clear(&return_value);
	atomic_clear(&fault_reason);
	finished_source = 0;
	memset(&control_clock, 0, sizeof(control_clock));
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
	application_record_reset();
	k_thread_create(&application_thread, application_stack,
			application_stack_size, entry, first, second, third,
			K_LOWEST_APPLICATION_THREAD_PRIO, K_USER, K_FOREVER);
	k_thread_heap_assign(&application_thread, NULL);
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

/* Keep thread-start/status scratch off the deeper ELF link call chain. */
static __noinline int start_native(const void *function, size_t length, size_t heap_size)
{
	return start_thread(native_entry, extension, (void *)function, NULL,
		(struct application_status){.format = APPLICATION_NATIVE, .image_bytes = length, .program_id = -1,
			.loader_bytes = heap_size, .loaded_bytes = extension->alloc_size}, false);
}

/* One LLEXT implementation serves either native context. Backing has already
 * been reserved; no application can observe a partially linked image. */
static int load_native(struct llext_loader *loader, size_t heap_size,
	struct k_mem_domain *domain, const void **function)
{
	int err = llext_heap_init(native_memory, heap_size);
	if (err) return err;
	loader_initialized = true;
	struct llext_load_param parameters = LLEXT_LOAD_PARAM_DEFAULT;
	err = llext_load(loader, "application", &extension, &parameters);
	if (err) return err;
	*function = llext_find_sym(&extension->exp_tab, "app_main");
	uintptr_t pc = (uintptr_t)*function & ~1U;
	uintptr_t text = (uintptr_t)extension->mem[LLEXT_MEM_TEXT];
	if (!*function || pc < text || pc - text >= extension->mem_size[LLEXT_MEM_TEXT]) return -ENOEXEC;
	return llext_add_domain(extension, domain);
}

/* lifecycle is held and this loader has passed native_image_check. */
static int run_image(struct llext_loader *loader, size_t length,
	const struct native_image_requirements *requirements)
{
	size_t heap_size = requirements->heap_size;
	/* Admission must not stop the current program when the shared heap is full. */
	void *memory = k_malloc(heap_size);
	if (!memory) return -ENOMEM;
	int err = prepare(MAX(requirements->stack_size, 2048));
	if (err) {
		k_free(memory);
		return err;
	}
	native_memory = memory;
	const void *function;
	err = load_native(loader, heap_size, &application_domain, &function);
	if (!err) err = start_native(function, length, heap_size);
	if (err) (void)release();
	return err;
}

#ifdef CONFIG_BRICKPHYR_PYTHON
/* The primary VM is blocked in control_start. lifecycle protects its source
 * image until this copy/link finishes, or cancels the pending request on stop. */
static int start_controller(void)
{
	struct native_image_requirements requirements;
	int err = native_image_check_buffer(control_request.image, control_request.size, &requirements);
	if (err) return err;
	controller = k_calloc(1, sizeof(*controller) + control_request.length);
	if (!controller) return -ENOMEM;
	controller->stack_size = MAX(requirements.stack_size, 2048);
	controller->loader_size = requirements.heap_size;
	controller->parameter_size = control_request.length;
	controller->revision = 1;
	if (control_request.length) memcpy(controller->parameters, control_request.parameters, control_request.length);
	struct k_thread *caller = k_current_get();
	struct k_heap *pool = caller->resource_pool;
	k_thread_system_pool_assign(caller);
	controller->stack = k_thread_stack_alloc(controller->stack_size, K_USER);
	controller->thread = k_object_alloc(K_OBJ_THREAD);
	k_thread_heap_assign(caller, pool);
	if (!controller->stack || !controller->thread) {
		err = -ENOMEM;
		goto failed;
	}
	err = k_mem_domain_init(&controller->domain, 0, NULL);
	if (err) goto failed;
	controller->domain_initialized = true;
	native_memory = k_malloc(requirements.heap_size);
	if (!native_memory) {
		err = -ENOMEM;
		goto failed;
	}
	struct llext_buf_loader loader = LLEXT_TEMPORARY_BUF_LOADER(control_request.image, control_request.size);
	const void *function;
	err = load_native(&loader.loader, requirements.heap_size, &controller->domain, &function);
	if (err) goto failed;
	memset(&control_clock, 0, sizeof(control_clock));
	k_thread_create(controller->thread, controller->stack, controller->stack_size,
		native_entry, extension, (void *)function, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO - 1, K_USER, K_FOREVER);
	if (IS_ENABLED(CONFIG_THREAD_NAME)) (void)k_thread_name_set(controller->thread, "app-control");
	k_thread_heap_assign(controller->thread, NULL);
	err = k_mem_domain_add_thread(&controller->domain, controller->thread);
	if (err) {
		k_thread_abort(controller->thread);
		k_thread_join(controller->thread, K_FOREVER);
		goto failed;
	}
	controller->started = true;
	k_thread_start(controller->thread);
	return 0;
failed:
	/* A rejected controller leaves Python running and preserves its first error. */
	int cleanup = release_controller();
	if (cleanup && !status.cleanup_error) status.cleanup_error = cleanup;
	return err;
}
#endif

#ifdef CONFIG_BRICKPHYR_PYTHON
static int run_python(const void *program, size_t length, enum application_format format,
	bool soft_reset, int program_id)
{
	int err = prepare(CONFIG_BRICKPHYR_APPLICATION_STACK_SIZE);
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
		python_stack_bottom = (uintptr_t)K_THREAD_STACK_BUFFER(application_stack);
		err = start_thread(python_entry, retained, (void *)length, (void *)(uintptr_t)format,
			(struct application_status){.format = format, .image_bytes = length, .repl = !program,
				.program_id = program_id,
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
		err = run_python(NULL, 0, APPLICATION_PYTHON, false, 128);
	}
	k_mutex_unlock(&lifecycle);
	return err;
}
#endif

/* The linker's section cache is needed only for loading, not admission. */
static __noinline int run_native_buffer(const void *image, size_t length,
	const struct native_image_requirements *requirements)
{
	struct llext_buf_loader loader = LLEXT_TEMPORARY_BUF_LOADER(image, length);
	return run_image(&loader.loader, length, requirements);
}

int application_run_native(const void *image, size_t length)
{
	if (!image || !length) {
		return -EINVAL;
	}
	struct native_image_requirements requirements;
	int err = native_image_check_buffer(image, length, &requirements);
	if (!err) {
		k_mutex_lock(&lifecycle, K_FOREVER);
		err = run_native_buffer(image, length, &requirements);
		k_mutex_unlock(&lifecycle);
	}
	return err;
}

#ifdef CONFIG_BRICKPHYR_APP_STORAGE
int application_write_upload(const char *path)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	expire_upload();
	if (!upload_requirements.heap_size || (upload_format != APPLICATION_NATIVE && upload_format != APPLICATION_FILES)) {
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

static int read_file(void *context, size_t offset, void *data, size_t size)
{
	struct fs_file_t *file = context;
	int err = fs_seek(file, offset, FS_SEEK_SET);
	if (err) return err;
	ssize_t count = fs_read(file, data, size);
	return count < 0 ? count : count == size ? 0 : -ENOEXEC;
}

/* The caller's store lock preserves the file during both admission and loading.
 * Native ELF keeps its streaming loader; Python snapshots its complete namespace. */
static int read_stored(const char *path, size_t length, void **image,
	struct native_image_requirements *requirements)
{
	*image = NULL;
	struct fs_file_t file;
	fs_file_t_init(&file);
	int err = fs_open(&file, path, FS_O_READ);
	if (err) {
		return err;
	}
	uint8_t magic[4];
	err = length < sizeof(magic) ? -ENOEXEC : read_file(&file, 0, magic, sizeof(magic));
	if (!err && !memcmp(magic, "\x7f" "ELF", sizeof(magic))) {
		err = native_image_check(read_file, &file, length, requirements);
	} else if (!err) {
#ifdef CONFIG_BRICKPHYR_PYTHON
		if (length > APPLICATION_IMAGE_LIMIT) {
			err = -EFBIG;
		} else if (!(*image = k_malloc(length))) {
			err = -ENOMEM;
		} else {
			err = read_file(&file, 0, *image, length);
			if (!err) {
				err = python_image_check(*image, length, APPLICATION_FILES);
			}
		}
#else
		err = -ENOEXEC;
#endif
	}
	int closed = fs_close(&file);
	return err ? err : closed;
}

int application_check_file(const char *path, size_t length)
{
	void *image;
	struct native_image_requirements requirements;
	int err = read_stored(path, length, &image, &requirements);
	k_free(image);
	return err;
}

/* Do not retain the directory-entry name buffer across validation or linking. */
static __noinline int stored_size(const char *path, size_t *length)
{
	struct fs_dirent info;
	int err = fs_stat(path, &info);
	if (err) {
		return err;
	}
	if (info.type != FS_DIR_ENTRY_FILE) {
		return -EINVAL;
	}
	*length = info.size;
	return 0;
}

/* Keep the linker's cache off the file admission and Python paths. */
static __noinline int run_native_file(const char *path, size_t length,
	const struct native_image_requirements *requirements)
{
	struct llext_fs_loader loader = LLEXT_FS_LOADER(path);
	return run_image(&loader.loader, length, requirements);
}

int application_run_file(const char *path, int program_id)
{
	size_t length;
	int err = stored_size(path, &length);
	if (err) {
		return err;
	}
	void *image;
	struct native_image_requirements requirements;
	err = read_stored(path, length, &image, &requirements);
	if (!err) {
		k_mutex_lock(&lifecycle, K_FOREVER);
#ifdef CONFIG_BRICKPHYR_PYTHON
		if (image) {
			err = run_python(image, length, APPLICATION_FILES, false, program_id);
		} else
#endif
		{
			err = run_native_file(path, length, &requirements);
		}
		k_mutex_unlock(&lifecycle);
	}
	k_free(image);
	return err;
}
#endif

static int complete_upload(void)
{
#ifdef CONFIG_BRICKPHYR_PYTHON
	int err;
	if (upload_format == APPLICATION_PYBRICKS) {
		size_t size;
		err = python_pybricks_package(upload, upload_bytes, upload_capacity, &size);
		if (err) {
			return err;
		}
		/* This only shrinks: Zephyr releases the suffix in place, without
		 * copying the image under the heap spinlock or retaining growth slack. */
		void *image = k_realloc(upload, size);
		if (!image) {
			return -ENOMEM;
		}
		upload = image;
		upload_capacity = upload_size = upload_bytes = size;
		upload_format = APPLICATION_FILES;
	}
	if (upload_format != APPLICATION_NATIVE) {
		err = python_image_check(upload, upload_size, upload_format);
		if (!err) {
			upload_requirements = (struct native_image_requirements){
				.heap_size = sizeof(execution_memory),
				.stack_size = CONFIG_BRICKPHYR_APPLICATION_STACK_SIZE};
		}
		return err;
	}
#endif
	return native_image_check_buffer(upload, upload_size, &upload_requirements);
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
	size_t padding = 0;
#ifdef CONFIG_BRICKPHYR_PYTHON
	padding = format == APPLICATION_PYBRICKS ? PYTHON_PYBRICKS_HEADROOM : 0;
	if (padding && total > PYTHON_PYBRICKS_LIMIT) {
		return -EFBIG;
	}
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
		/* Reserve normalization space while receiving, before replacing the
		 * previous image. No second complete image is needed at completion. */
		size_t capacity = open_size && length <= bound / 2 ? length : bound;
		capacity += padding;
		void *data_buffer = k_malloc(capacity);
		if (!data_buffer) {
			err = -ENOMEM;
			goto out;
		}
		discard_upload();
		upload = data_buffer;
		upload_capacity = capacity;
		upload_size = total;
		upload_format = format;
		upload_pybricks = format == APPLICATION_PYBRICKS;
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
		if (offset + length + padding > upload_capacity) {
			/* Unknown-length Pybricks transfers grow geometrically instead of
			 * reserving the maximum image for every small program. Copy outside
			 * the heap lock: k_realloc may copy with interrupts masked. */
			size_t capacity = MAX(offset + length, (upload_capacity - padding) * 2);
			/* Packet-sized growth must not stop just below the limit and then
			 * need two almost-full buffers for the final few bytes. */
			if (capacity > bound / 2) {
				capacity = bound;
			}
			capacity += padding;
			void *buffer = k_malloc(capacity);
			if (!buffer && capacity > offset + length + padding) {
				/* Optional growth headroom must not reject a fragment that fits. */
				capacity = offset + length + padding;
				buffer = k_malloc(capacity);
			}
			if (!buffer) {
				/* Keep the accepted bytes and offset available for a retry. */
				err = -ENOMEM;
				goto out;
			}
			memcpy(buffer, upload, upload_bytes);
			k_free(upload);
			upload = buffer;
			upload_capacity = capacity;
		}
		memcpy((uint8_t *)upload + offset, data, length);
		upload_bytes += length;
	}
	if (upload_size && upload_bytes == upload_size && !upload_requirements.heap_size) {
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
	if (upload_requirements.heap_size) {
#ifdef CONFIG_BRICKPHYR_PYTHON
		if (upload_format != APPLICATION_NATIVE) {
			err = run_python(upload, upload_size, upload_format, false, upload_pybricks ? 0 : -1);
		} else
#endif
		{
			err = run_native_buffer(upload, upload_size, &upload_requirements);
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
	value->upload_ready = upload_requirements.heap_size != 0;
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

size_t application_read_output(void *data, size_t capacity, struct application_status *value)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	(void)reap(false);
	size_t count = application_output(data, capacity);
	copy_status(value);
	k_mutex_unlock(&lifecycle);
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
