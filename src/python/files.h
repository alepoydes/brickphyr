// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include "py/reader.h"
#include "python_source.h"

int python_files_check(const void *image, size_t length);
void python_files_set_source(const struct python_source *source);
struct python_resource python_files_resource(const char *path);
void python_reader_new_source(mp_reader_t *reader, const struct python_source *source,
	struct python_resource file, bool bytecode);
const char *python_files_entry(void);
void python_reader_new_mem(mp_reader_t *reader, const void *data, size_t length);
