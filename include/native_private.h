// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <zephyr/toolchain.h>
#include <zephyr/syscall.h>
__syscall void native_finished(int result);
#include <zephyr/syscalls/native_private.h>
