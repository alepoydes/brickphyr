// SPDX-License-Identifier: Apache-2.0
#include "app_package.h"
#include "python_runtime.h"
#include <errno.h>
#include <string.h>
#include <zephyr/sys/util.h>

int python_resource_check(const struct app_source *source, struct app_resource file,
	enum application_format format)
{
	if (format != APPLICATION_PYTHON && format != APPLICATION_MPY) return 0;
	uint8_t buffer[128];
	if (format == APPLICATION_MPY) {
		if (file.length < 4) return -ENOEXEC;
		int err = app_source_read(source, file.offset, buffer, 4);
		return err ? err : python_image_check(buffer, 4, APPLICATION_MPY);
	}
	while (file.length) {
		size_t size = MIN(file.length, sizeof(buffer));
		int err = app_source_read(source, file.offset, buffer, size);
		if (err) return err;
		if (memchr(buffer, 0, size)) return -ENOEXEC;
		file.offset += size;
		file.length -= size;
	}
	return 0;
}

int python_source_check(const struct app_source *source)
{
	struct app_package_info info;
	int err = app_package_check(source, python_resource_check, &info);
	return err ? err : info.format == APPLICATION_NATIVE ? -ENOTSUP : 0;
}
