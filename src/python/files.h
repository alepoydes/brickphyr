// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include "py/reader.h"

int python_files_check(const void *image, size_t length);
void python_files_set(const void *image, size_t length);
const char *python_files_entry(void);
void python_reader_new_mem(mp_reader_t *reader, const void *data, size_t length);
