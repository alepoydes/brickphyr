// SPDX-License-Identifier: Apache-2.0
#include "native.h"
#include "native_image.h"
#include "native_private.h"
#include "hub.h"
#include "hub_power.h"

#include <brickphyr.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <zephyr/app_memory/mem_domain.h>
#include <zephyr/fatal.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/kernel.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/llext/llext.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>

/* A timer wakeup can reset equal-priority time slicing. Control and diagnostics
 * must preempt the application, even when its entry never yields. */
BUILD_ASSERT(CONFIG_SHELL_THREAD_PRIORITY_OVERRIDE &&
	     CONFIG_SHELL_THREAD_PRIORITY < K_LOWEST_APPLICATION_THREAD_PRIO);
BUILD_ASSERT(CONFIG_LOG_PROCESS_THREAD_CUSTOM_PRIORITY &&
	     CONFIG_LOG_PROCESS_THREAD_PRIORITY < K_LOWEST_APPLICATION_THREAD_PRIO);

K_MUTEX_DEFINE(lifecycle);
/* A single application stack: no thread creation API or inherited objects. */
K_THREAD_STACK_DEFINE(application_stack, 2048);
static struct k_thread application_thread;
static struct k_mem_domain application_domain;
static bool domain_initialized;
static struct llext *extension;
static void *loader_memory;
static struct native_status status;
static atomic_t return_value;
static atomic_t fault_reason;
static atomic_t active;
#define OUTPUT_CAPACITY 1024
RING_BUF_DECLARE(output, OUTPUT_CAPACITY);
static struct k_spinlock output_lock;
static uint32_t output_dropped;

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
	if (length > 128) {
		return -EMSGSIZE;
	}
	k_spinlock_key_t key = k_spin_lock(&output_lock);
	size_t count = ring_buf_put(&output, data, length);
	output_dropped = MIN((uint64_t)output_dropped + length - count, UINT32_MAX);
	k_spin_unlock(&output_lock, key);
	return count;
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

/* Completion is a private syscall used by the user-mode trampoline. */
void z_impl_native_finished(int result)
{
	atomic_set(&return_value, result);
}

static inline void z_vrfy_native_finished(int result)
{
	K_OOPS(K_SYSCALL_VERIFY(k_sched_current_thread_query() == &application_thread));
	z_impl_native_finished(result);
}
#include <zephyr/syscalls/native_finished_mrsh.c>

static void entry(void *ext, void *function, void *unused)
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
	native_finished(result);
}

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
	/* A domain is initialized once. Remove public partition descriptors only
	 * after joining the thread, before freeing the memory they describe. */
	int err = 0;
	if (domain_initialized) {
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
	if (!err && loader_memory) {
		err = llext_heap_uninit();
		if (!err) {
			k_free(loader_memory);
			loader_memory = NULL;
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
	status.result = atomic_get(&return_value);
	int fault = atomic_get(&fault_reason);
	status.fault = fault ? fault - 1 : 0;
	status.state = fault ? NATIVE_FAULT : ended ? NATIVE_RETURNED : NATIVE_STOPPED;
	(void)k_thread_stack_space_get(&application_thread, &status.stack_unused);
	int err = release();
	return err;
}

int native_stop(void)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	int err = reap(true);
	k_mutex_unlock(&lifecycle);
	return err;
}

int native_shutdown(void)
{
	hub_power_request_shutdown();
	return native_stop();
}

void native_poll(void)
{
	if (k_mutex_lock(&lifecycle, K_NO_WAIT)) {
		return;
	}
	(void)reap(false);
	k_mutex_unlock(&lifecycle);
}

int native_run(const void *image, size_t length)
{
	if (!image || !length) {
		return -EINVAL;
	}
	struct llext_buf_loader loader = LLEXT_TEMPORARY_BUF_LOADER(image, length);
	size_t heap_size;
	int err = native_image_check(&loader.loader, length, &heap_size);
	if (err) {
		return err;
	}
	k_mutex_lock(&lifecycle, K_FOREVER);
	err = reap(true);
	if (err) {
		goto out;
	}
	if (!hub_power_application(true)) {
		err = -ESHUTDOWN;
		goto out;
	}
	loader_memory = k_malloc(heap_size);
	if (!loader_memory) {
		err = -ENOMEM;
		goto failed;
	}
	err = llext_heap_init(loader_memory, heap_size);
	if (err) {
		k_free(loader_memory);
		loader_memory = NULL;
		goto failed;
	}
	struct llext_load_param parameters = LLEXT_LOAD_PARAM_DEFAULT;
	err = llext_load(&loader.loader, "application", &extension, &parameters);
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
	if (!domain_initialized) {
		err = k_mem_domain_init(&application_domain, 0, NULL);
		domain_initialized = err == 0;
	}
	if (!err) {
		err = llext_add_domain(extension, &application_domain);
	}
	if (err) {
		goto failed;
	}
	status = (struct native_status){
		.state = NATIVE_RUNNING, .image_bytes = length, .loader_bytes = heap_size,
		.loaded_bytes = extension->alloc_size, .stack_bytes = sizeof(application_stack),
	};
	atomic_clear(&return_value);
	atomic_clear(&fault_reason);
	k_spinlock_key_t key = k_spin_lock(&output_lock);
	ring_buf_reset(&output);
	output_dropped = 0;
	k_spin_unlock(&output_lock, key);
	k_thread_create(&application_thread, application_stack,
			K_THREAD_STACK_SIZEOF(application_stack), entry, extension, (void *)function, NULL,
			K_LOWEST_APPLICATION_THREAD_PRIO, K_USER, K_FOREVER);
	err = k_mem_domain_add_thread(&application_domain, &application_thread);
	if (err) {
		k_thread_abort(&application_thread);
		k_thread_join(&application_thread, K_FOREVER);
		goto failed;
	}
	atomic_set(&active, 1);
	k_thread_start(&application_thread);
	goto out;
failed:
	/* Retain the original loading error; a failed release retains ownership. */
	(void)release();
out:
	k_mutex_unlock(&lifecycle);
	return err;
}

void native_get_status(struct native_status *value)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	(void)reap(false);
	*value = status;
	k_spinlock_key_t key = k_spin_lock(&output_lock);
	value->output_dropped = output_dropped;
	k_spin_unlock(&output_lock, key);
	k_mutex_unlock(&lifecycle);
}

size_t native_output(void *data, size_t capacity)
{
	k_spinlock_key_t key = k_spin_lock(&output_lock);
	size_t count = ring_buf_get(&output, data, capacity);
	k_spin_unlock(&output_lock, key);
	return count;
}

static int cmd_app(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "stop")) {
		int err = native_stop();
		shell_print(shell, "Stop: %d", err);
		return err;
	}
	if (argc == 2 && !strcmp(argv[1], "output")) {
		char data[129];
		size_t count;
		size_t remaining = OUTPUT_CAPACITY;
		while (remaining && (count = native_output(data, MIN(sizeof(data) - 1, remaining)))) {
			data[count] = 0;
			shell_fprintf(shell, SHELL_NORMAL, "%s", data);
			remaining -= count;
		}
		return 0;
	}
	if (argc != 1 && (argc != 2 || strcmp(argv[1], "status"))) {
		shell_error(shell, "Usage: app [status | stop | output]");
		return -EINVAL;
	}
	struct native_status value;
	native_get_status(&value);
	static const char *const states[] = {"idle", "running", "returned", "stopped", "fault"};
	shell_print(shell, "Application: %s; result %d; fault %u; cleanup %d", states[value.state],
		    value.result, value.fault, value.cleanup_error);
	shell_print(shell, "Image %zu; loader budget %zu; loaded %zu; stack %zu (unused %zu)",
		    value.image_bytes, value.loader_bytes, value.loaded_bytes,
		    value.stack_bytes, value.stack_unused);
	shell_print(shell, "Output dropped: %u; ABI %s", value.output_dropped, BRICKPHYR_ABI);
	return 0;
}

SHELL_CMD_ARG_REGISTER(app, NULL, "Native application status, stop and output.", cmd_app, 1, 1);
