# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include ${APPLICATION_SOURCE_DIR}/sdk/include)
zephyr_library_sources(board.c)
