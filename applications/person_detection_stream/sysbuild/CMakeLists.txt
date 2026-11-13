#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sysbuild LANGUAGES)

set(MERGED_HEX_APP_IMAGE ${DEFAULT_IMAGE})
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/merged_hex.cmake)
