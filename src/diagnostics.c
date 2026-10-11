// SPDX-License-Identifier: Apache-2.0
#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>

/* Always available, including with no computer. No backend callback waits for
 * a transport. Preserve the first messages until read, and count overflow. */
#define DIAGNOSTIC_CAPACITY 2048
RING_BUF_DECLARE(messages, DIAGNOSTIC_CAPACITY);
static struct k_spinlock lock;
static uint32_t dropped_bytes;
static uint32_t dropped_messages;
static uint8_t format_buffer[128];

static int write_message(uint8_t *data, size_t length, void *context)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	size_t count = ring_buf_put(&messages, data, length);
	dropped_bytes = MIN((uint64_t)dropped_bytes + length - count, UINT32_MAX);
	k_spin_unlock(&lock, key);
	return length;
}

LOG_OUTPUT_DEFINE(formatted, write_message, format_buffer, sizeof(format_buffer));

static void process(const struct log_backend *backend, union log_msg_generic *message)
{
	log_output_msg_process(&formatted, &message->log, LOG_OUTPUT_FLAG_LEVEL);
}

static void dropped(const struct log_backend *backend, uint32_t count)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	dropped_messages = MIN((uint64_t)dropped_messages + count, UINT32_MAX);
	k_spin_unlock(&lock, key);
}

static void panic(const struct log_backend *backend)
{
	/* The sink is already bounded and synchronous in the logger's context. */
	log_output_flush(&formatted);
}

static const struct log_backend_api backend_api = {
	.process = process, .dropped = dropped, .panic = panic,
};

LOG_BACKEND_DEFINE(brickphyr_diagnostics, backend_api, true);

static int cmd_diagnostics(const struct shell *shell, size_t argc, char **argv)
{
	char data[129];
	size_t remaining = DIAGNOSTIC_CAPACITY;
	while (remaining) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		size_t count = ring_buf_get(&messages, data, MIN(remaining, sizeof(data) - 1));
		k_spin_unlock(&lock, key);
		if (!count) {
			break;
		}
		data[count] = 0;
		shell_fprintf(shell, SHELL_NORMAL, "%s", data);
		remaining -= count;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t bytes = dropped_bytes;
	uint32_t records = dropped_messages;
	k_spin_unlock(&lock, key);
	shell_print(shell, "Dropped diagnostic bytes: %u; messages: %u", bytes, records);
	return 0;
}

SHELL_CMD_ARG_REGISTER(diagnostics, NULL, "Read bounded firmware diagnostics and overflow counters.",
		      cmd_diagnostics, 1, 0);
