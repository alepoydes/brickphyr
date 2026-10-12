// SPDX-License-Identifier: Apache-2.0
#include "python_runtime.h"
#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>
#include <zephyr/app_memory/mem_domain.h>

static ZTEST_BMEM uint8_t heap[64 * 1024] __aligned(16);
static ZTEST_BMEM char output[2048];
static ZTEST_BMEM size_t output_size;
static const uint8_t calculation[] = {
#include <calculation.inc>
};
static const uint8_t reference[] = {
#include <reference.inc>
};

/* This isolated fixture has no host input; interactive behavior uses the
 * production application's queues and SMP handlers in test_python_io.py. */
int mp_hal_stdin_rx_chr(void) { return 4; }
uintptr_t mp_hal_stdio_poll(uintptr_t flags) { return flags & 4; }
uintptr_t mp_hal_ticks_ms(void) { return k_uptime_get_32(); }
void python_event_wait(int64_t timeout_ms) { k_msleep(timeout_ms < 0 ? 1 : timeout_ms); }

uintptr_t mp_hal_stdout_tx_strn(const char *data, size_t size)
{
	size_t count = MIN(size, sizeof(output) - output_size - 1);
	memcpy(output + output_size, data, count);
	output_size += count;
	output[output_size] = 0;
	return count;
}

static int execute(const void *program, size_t length, bool bytecode)
{
#ifdef CONFIG_USERSPACE
	zassert_true(k_is_user_context());
#endif
	output_size = 0;
	output[0] = 0;
	/* Leave 4 KiB of the 16 KiB ZTEST stack for the test framework. */
	volatile uintptr_t stack_top;
	return python_execute(program, length, bytecode ? APPLICATION_MPY : APPLICATION_PYTHON,
		heap, sizeof(heap),
		(uintptr_t)&stack_top - 12 * 1024);
}

static int run(const char *source)
{
	return execute(source, strlen(source), false);
}

ZTEST_USER(python, test_source_and_gc)
{
	zassert_ok(run("import gc\n"
		"import math\n"
		"assert math.sqrt(9) == 3.0\n"
		"data = [str(n) for n in range(100)]\n"
		"for n in range(100):\n"
		"    garbage = bytearray(1024)\n"
		"    gc.collect()\n"
		"assert data[99] == '99'\n"
		"print(sum(range(10)), 2 ** 80)\n"));
	zassert_str_equal(output, "45 1208925819614629174706176\r\n");
}

ZTEST_USER(python, test_exceptions_and_fresh_state)
{
	zassert_ok(run("previous = 12\n"));
	zassert_equal(run("print(previous)\n"), -ENOEXEC);
	zassert_not_null(strstr(output, "NameError"));
	zassert_equal(run("if :\n"), -ENOEXEC);
	zassert_not_null(strstr(output, "SyntaxError"));
	zassert_equal(run("raise ValueError('example')\n"), -ENOEXEC);
	zassert_not_null(strstr(output, "ValueError: example"));
	zassert_equal(run("bytearray(1000000)\n"), -ENOEXEC);
	zassert_not_null(strstr(output, "MemoryError"));
	zassert_ok(run("print('recovered')\n"));
	zassert_str_equal(output, "recovered\r\n");
}

ZTEST_USER(python, test_bytecode_and_truncation)
{
	zassert_ok(execute(calculation, sizeof(calculation), true));
	zassert_str_equal(output, "6765\r\n");
	zassert_ok(execute(reference, sizeof(reference), true));
	zassert_str_equal(output, "6765\r\n");
	/* Every incomplete prefix must fail without hanging the loader. */
	for (size_t length = 0; length < sizeof(calculation); length++) {
		zassert_equal(execute(calculation, length, true), -ENOEXEC, "prefix %zu", length);
	}
	zassert_ok(run("print('after bytecode error')\n"));
}

static void *setup(void)
{
#if defined(CONFIG_USERSPACE) && !defined(CONFIG_NEED_LIBC_MEM_PARTITION)
	zassert_ok(k_mem_domain_add_partition(&k_mem_domain_default, &python_partition));
#endif
	return NULL;
}

ZTEST_SUITE(python, NULL, setup, NULL, NULL, NULL);
