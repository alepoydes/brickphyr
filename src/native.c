// SPDX-License-Identifier: Apache-2.0
#include "native.h"
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
#include "app_store.h"
#include <zephyr/llext/fs_loader.h>
#endif
#include "native_image.h"
#include "native_private.h"
#include "hub.h"
#include "hub_matrix.h"
#include "hub_sound.h"
#include "hub_imu.h"
#include "hub_battery.h"
#include "hub_ports.h"
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
static void *upload;
static size_t upload_size;
static size_t upload_bytes;
static size_t upload_heap;
static atomic_t session_generation;
static atomic_val_t upload_generation;
static bool shutting_down;

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

void native_session_closed(void)
{
	atomic_inc(&session_generation);
}

void native_discard_upload(void)
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
	int err = hub_ports_release(BP_PORT_APPLICATION);
	/* A domain is initialized once. Remove public partition descriptors only
	 * after joining the thread, before freeing the memory they describe. */
	int sound_error = hub_sound_release(BP_SOUND_APPLICATION);
	if (!err) err = sound_error;
	int matrix_error = hub_matrix_release(BP_MATRIX_APPLICATION);
	if (!err) {
		err = matrix_error;
	}
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
	k_mutex_lock(&lifecycle, K_FOREVER);
	shutting_down = true;
	int err = reap(true);
	discard_upload();
	k_mutex_unlock(&lifecycle);
	return err;
}

void native_poll(void)
{
	if (k_mutex_lock(&lifecycle, K_NO_WAIT)) {
		return;
	}
	(void)reap(false);
	expire_upload();
	k_mutex_unlock(&lifecycle);
}

/* lifecycle is held and this loader has passed native_image_check. */
static int run_image(struct llext_loader *loader, size_t length, size_t heap_size)
{
	int err = reap(true);
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
	return err;
}

int native_run(const void *image, size_t length)
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
int native_run_file(const char *path)
{
	struct fs_dirent info;
	int err = fs_stat(path, &info);
	if (err) {
		return err;
	}
	if (info.type != FS_DIR_ENTRY_FILE) {
		return -EINVAL;
	}
	struct llext_fs_loader loader = LLEXT_FS_LOADER(path);
	size_t heap;
	err = loader.loader.prepare(&loader.loader);
	if (err) {
		return err;
	}
	err = native_image_check(&loader.loader, info.size, &heap);
	loader.loader.finalize(&loader.loader);
	/* The store lock keeps the checked path unchanged until loading finishes.
	 * LLEXT owns prepare/finalize for the second pass; no full-image RAM copy. */
	if (!err) {
		k_mutex_lock(&lifecycle, K_FOREVER);
		err = run_image(&loader.loader, info.size, heap);
		k_mutex_unlock(&lifecycle);
	}
	return err;
}
#endif

int native_upload(size_t offset, size_t total, const void *data, size_t length, size_t *next)
{
	if (!data || !next || !length || !total || offset > total || length > total - offset) {
		return -EINVAL;
	}
	if (total > NATIVE_IMAGE_LIMIT) {
		return -EFBIG;
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
		void *data_buffer = k_malloc(total);
		if (!data_buffer) {
			err = -ENOMEM;
			goto out;
		}
		discard_upload();
		upload = data_buffer;
		upload_size = total;
		upload_generation = atomic_get(&session_generation);
	}
	if (!upload || total != upload_size || offset > upload_bytes) {
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
	if (upload_bytes == upload_size && !upload_heap) {
		struct llext_buf_loader loader = LLEXT_TEMPORARY_BUF_LOADER(upload, upload_size);
		err = native_image_check(&loader.loader, upload_size, &upload_heap);
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

int native_run_uploaded(void)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	expire_upload();
	int err = -ENODATA;
	if (upload_heap) {
		struct llext_buf_loader loader = LLEXT_TEMPORARY_BUF_LOADER(upload, upload_size);
		err = run_image(&loader.loader, upload_size, upload_heap);
		if (!err) {
			/* The temporary buffer loader copies all retained sections. */
			discard_upload();
		}
	}
	k_mutex_unlock(&lifecycle);
	return err;
}

/* Caller holds lifecycle; reading output state never consumes it. */
static void copy_status(struct native_status *value)
{
	*value = status;
	value->upload_bytes = upload_bytes;
	value->upload_size = upload_size;
	value->upload_ready = upload_heap != 0;
	k_spinlock_key_t key = k_spin_lock(&output_lock);
	value->output_dropped = output_dropped;
	value->output_bytes = ring_buf_size_get(&output);
	k_spin_unlock(&output_lock, key);
}

int native_observe(struct native_status *value)
{
	if (k_mutex_lock(&lifecycle, K_NO_WAIT)) {
		return -EBUSY;
	}
	copy_status(value);
	k_mutex_unlock(&lifecycle);
	return 0;
}

void native_get_status(struct native_status *value)
{
	k_mutex_lock(&lifecycle, K_FOREVER);
	(void)reap(false);
	expire_upload();
	copy_status(value);
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
		int err = native_run_uploaded();
		shell_print(shell, "Run: %d", err);
		return err;
	}
	if (argc == 2 && !strcmp(argv[1], "discard")) {
		native_discard_upload();
		return 0;
	}
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
		shell_error(shell, "Usage: app [status | run [name] | list | remove name | stop | output | discard]");
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
	shell_print(shell, "Upload %zu/%zu; %s", value.upload_bytes, value.upload_size,
		    !value.upload_size ? "empty" : value.upload_ready ? "ready" : "incomplete");
	return 0;
}

SHELL_CMD_ARG_REGISTER(app, NULL, "Native application status, stop and output.", cmd_app, 1, 2);
