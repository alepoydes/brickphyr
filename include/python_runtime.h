// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "application.h"
#include "python_source.h"

#define PYTHON_FILE_LIMIT 64
#define PYTHON_PATH_LIMIT 128
#define PYTHON_STACK_MIN 4096
/* Maximum expansion of the reference module records into the common file map. */
#define PYTHON_PYBRICKS_HEADROOM (16 * (PYTHON_FILE_LIMIT + 1))
#define PYTHON_PYBRICKS_LIMIT (APPLICATION_IMAGE_LIMIT - PYTHON_PYBRICKS_HEADROOM)

/* Admission checks do not compile, allocate or execute application input. */
int python_image_check(const void *program, size_t length, enum application_format format);
/* Normalize module records in place, with PYTHON_PYBRICKS_HEADROOM spare bytes.
 * The caller discards failed conversions and admits successful output with the
 * same python_image_check as other FILES uploads. No second image is allocated. */
int python_pybricks_package(void *data, size_t length, size_t capacity, size_t *size);
/* A host request sets a signal only; the VM handles it at its own safe points. */
void python_input_reset(bool soft_reset, bool repl);
void python_interrupt(void);
void python_stop(void);
int python_interrupt_char(void);
bool python_repl_reading(void);
/* Request a GC sample at the VM's next poll; return the previous complete
 * sample, with its age. Negative fields mean no sample is available yet. */
void python_memory_get(uint32_t now_ms, int *used, int *free, int *age_ms);

/* REPL Ctrl-D asks the supervisor to release the old application and restart. */
#define PYTHON_RESTART 1

/* Called by the application thread, with all buffers in its memory domain.
 * One VM at a time; the caller owns the heap and reclaims it after joining.
 * stack_bottom is the lowest usable address of the downward-growing stack.
 * A NULL program with length zero selects interactive REPL. */
int python_execute_source(const struct python_source *source, enum application_format format,
	void *heap, size_t heap_size, uintptr_t stack_bottom);
int python_execute(const void *program, size_t length, enum application_format format,
	void *heap, size_t heap_size, uintptr_t stack_bottom);

#ifdef CONFIG_USERSPACE
#ifdef CONFIG_NEED_LIBC_MEM_PARTITION
#include <zephyr/sys/libc-hooks.h>
#define python_partition z_libc_partition
#else
extern struct k_mem_partition python_partition;
#endif
#endif
