// SPDX-License-Identifier: MIT
// Copyright (c) 2022-2023 Damien P. George
// Execution entry adapted from MicroPython ports/embed/port/embed_util.c.
#include "python_runtime.h"
#include "files.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>

#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "py/persistentcode.h"
#include "py/reader.h"
#include "py/runtime.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"
#include "shared/readline/readline.h"
#ifdef CONFIG_BRICKPHYR_PYBRICKS
#include <pybricks/parameters.h>
#include <pybricks/tools.h>
#endif

static atomic_t interrupt_pending;
static atomic_t interrupt_character = 3;
static atomic_t repl_reading;
static atomic_t memory_sequence, memory_used, memory_free, memory_time;
static size_t memory_capacity;

void python_input_reset(bool soft_reset, bool repl)
{
	atomic_clear(&interrupt_pending);
	atomic_clear(&memory_sequence);
	/* Typeahead can precede the VM's first timeslice. A REPL control byte
	 * must already belong to readline, not raise outside its exception frame. */
	atomic_set(&interrupt_character, repl ? -1 : 3);
	atomic_set(&repl_reading, repl);
	if (!soft_reset) {
		pyexec_mode_kind = PYEXEC_MODE_FRIENDLY_REPL;
	}
}

bool python_repl_reading(void)
{
	return atomic_get(&repl_reading);
}

void mp_hal_stdio_mode_raw(void)
{
	atomic_set(&repl_reading, 1);
	mp_hal_set_interrupt_char(-1);
}

void mp_hal_stdio_mode_orig(void)
{
	atomic_clear(&repl_reading);
	mp_hal_set_interrupt_char(3);
}

void python_interrupt(void)
{
	atomic_set_bit(&interrupt_pending, 0);
}

void python_stop(void)
{
	atomic_set_bit(&interrupt_pending, 1);
}

int python_interrupt_char(void)
{
	return atomic_get(&interrupt_character);
}

void mp_hal_set_interrupt_char(int value)
{
	atomic_set(&interrupt_character, value);
}

void python_poll(void)
{
	atomic_val_t pending = atomic_set(&interrupt_pending, 0);
	if (pending & 4) {
		/* Only the VM scans its own GC state. No collection, allocation or
		 * supervisor suspension of a possibly interrupted allocator. */
		gc_info_t info;
		gc_info_fast(&info);
		atomic_inc(&memory_sequence);
		atomic_set(&memory_used, info.used);
		atomic_set(&memory_free, info.free);
		atomic_set(&memory_time, k_uptime_get_32());
		atomic_inc(&memory_sequence);
	}
	if (pending & 2) {
		/* Preallocate like KeyboardInterrupt, keeping traceback references
		 * visible to GC when application code catches and retains the error. */
		mp_obj_exception_clear_traceback(MP_STATE_PORT(python_stop_exception));
		mp_sched_exception(MP_STATE_PORT(python_stop_exception));
	} else if (pending & 1) {
		mp_sched_keyboard_interrupt();
	}
}

void python_memory_get(uint32_t now_ms, int *used, int *free, int *age_ms)
{
	/* Called by the supervisor: this user-compiled unit must not issue a
	 * forced user syscall on its caller's kernel-only stack. */
	*used = *free = *age_ms = -1;
	atomic_val_t sequence = atomic_get(&memory_sequence);
	if (sequence && !(sequence & 1)) {
		int allocated = atomic_get(&memory_used), available = atomic_get(&memory_free);
		uint32_t age = now_ms - (uint32_t)atomic_get(&memory_time);
		if (sequence == atomic_get(&memory_sequence) && allocated >= 0 && available >= 0 &&
		    allocated <= memory_capacity && available <= memory_capacity - allocated) {
			*used = allocated;
			*free = available;
			*age_ms = MIN(age, INT32_MAX);
		}
	}
	atomic_set_bit(&interrupt_pending, 2);
}

void mp_hal_delay_ms(mp_uint_t duration)
{
	mp_uint_t start = mp_hal_ticks_ms();
	for (;;) {
		mp_event_handle_nowait();
		mp_uint_t elapsed = mp_hal_ticks_ms() - start;
		if (elapsed >= duration) {
			return;
		}
		python_event_wait(duration - elapsed);
	}
}

int python_image_check(const void *program, size_t length, enum application_format format)
{
	if (!program) {
		return -EINVAL;
	}
	if (format == APPLICATION_FILES) {
		return python_files_check(program, length);
	}
	if (format == APPLICATION_PYTHON) {
		return memchr(program, 0, length) ? -ENOEXEC : 0;
	}
	if (format != APPLICATION_MPY) {
		return -ENOTSUP;
	}
	const uint8_t *header = program;
	if (length < 4 || header[0] != 'M' ||
	    header[3] > sizeof(mp_int_t) * 8 - 1) {
		return -ENOEXEC;
	}
	uint8_t arch = MPY_FEATURE_DECODE_ARCH(header[2]);
	if (!MPY_VERSION_TEST(header[1], arch)) {
		return -ENOTSUP;
	}
	if (arch != MP_NATIVE_ARCH_NONE &&
	    (!MPY_FEATURE_ARCH_TEST(arch) || MPY_FEATURE_DECODE_SUB_VERSION(header[2]) != MPY_SUB_VERSION)) {
		return -ENOTSUP;
	}
	/* C .mpy modules call float-valued runtime functions. A lower Thumb
	 * architecture may use the soft ABI even though its instructions run. */
	if (IS_ENABLED(CONFIG_FP_HARDABI) && arch != MP_NATIVE_ARCH_NONE &&
	    arch != MPY_FEATURE_ARCH) {
		return -ENOTSUP;
	}
	return 0;
}

int python_execute_source(const struct python_source *source, enum application_format format,
	void *heap, size_t heap_size, uintptr_t stack_bottom)
{
	volatile uintptr_t stack_top;
	if (!heap || heap_size < 4096 || stack_bottom >= (uintptr_t)&stack_top ||
	    (uintptr_t)&stack_top - stack_bottom <= MICROPY_STACK_CHECK_MARGIN) {
		return -EINVAL;
	}
	memset(&mp_state_ctx, 0, sizeof(mp_state_ctx));
	python_files_set_source(format == APPLICATION_FILES ? source : NULL);
	mp_cstack_init_with_top((void *)&stack_top, (uintptr_t)&stack_top - stack_bottom);
	gc_init(heap, (uint8_t *)heap + heap_size);
	memory_capacity = heap_size;
	int result = 0;
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		mp_init();
		MP_STATE_PORT(python_stop_exception) = mp_obj_new_exception(&mp_type_SystemExit);
		readline_init0();
#ifdef CONFIG_BRICKPHYR_PYBRICKS
		pb_type_Color_reset();
		pb_module_tools_init();
#endif
		if (!source) {
			for (;;) {
				int ret = pyexec_mode_kind == PYEXEC_MODE_RAW_REPL ?
					pyexec_raw_repl() : pyexec_friendly_repl();
				if (ret & PYEXEC_FORCED_EXIT) {
					result = PYTHON_RESTART;
					mp_hal_stdout_tx_str("MPY: soft reboot\r\n");
					break;
				}
			}
			nlr_pop();
			mp_deinit();
			return result;
		}
		qstr filename = format == APPLICATION_FILES ?
			qstr_from_str(python_files_entry()) : MP_QSTR__lt_stdin_gt_;
		if (format == APPLICATION_FILES) {
			mp_store_global(MP_QSTR___file__, MP_OBJ_NEW_QSTR(filename));
			mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(filename));
		}
		bool bytecode = format == APPLICATION_MPY ||
			(format == APPLICATION_FILES && !strcmp(qstr_str(filename) + qstr_len(filename) - 4, ".mpy"));
		mp_obj_t function;
		if (bytecode) {
			mp_reader_t reader;
			if (format == APPLICATION_FILES) {
				mp_reader_new_file(&reader, filename);
			} else {
				python_reader_new_source(&reader, source, (struct python_resource){0, source->length}, true);
			}
			mp_module_context_t *context = m_new_obj(mp_module_context_t);
			context->module.globals = mp_globals_get();
			mp_compiled_module_t module = {.context = context};
			mp_raw_code_load(&reader, &module);
			function = mp_make_function_from_proto_fun(module.rc, context, MP_OBJ_NULL);
		} else {
			mp_lexer_t *lexer;
			if (format == APPLICATION_FILES) {
				lexer = mp_lexer_new_from_file(filename);
			} else {
				mp_reader_t reader;
				python_reader_new_source(&reader, source, (struct python_resource){0, source->length}, false);
				lexer = mp_lexer_new(filename, reader);
			}
			qstr name = lexer->source_name;
			mp_parse_tree_t tree = mp_parse(lexer, MP_PARSE_FILE_INPUT);
			function = mp_compile(&tree, name, false);
		}
		mp_call_function_0(function);
		nlr_pop();
	} else {
		mp_obj_t exception = MP_OBJ_FROM_PTR(nlr.ret_val);
		if (!mp_obj_exception_match(exception, MP_OBJ_FROM_PTR(&mp_type_SystemExit))) {
			mp_obj_print_exception(&mp_plat_print, exception);
			result = -ENOEXEC;
		}
	}
	mp_deinit();
	return result;
}

int python_execute(const void *program, size_t length, enum application_format format,
	void *heap, size_t heap_size, uintptr_t stack_bottom)
{
	if (!program && length) return -EINVAL;
	const struct python_source source = {python_source_memory, (void *)program, length};
	return python_execute_source(program ? &source : NULL, format, heap, heap_size, stack_bottom);
}

void gc_collect(void)
{
	gc_collect_start();
	gc_helper_collect_regs_and_stack();
	gc_collect_end();
}

void nlr_jump_fail(void *value)
{
	ARG_UNUSED(value);
	k_oops();
	CODE_UNREACHABLE;
}

MP_REGISTER_ROOT_POINTER(mp_obj_t python_stop_exception);
