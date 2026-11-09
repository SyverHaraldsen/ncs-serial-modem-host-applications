#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project("Serial Modem Host - 91m1 (AT)")

set(SMHA_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)

zephyr_include_directories(
	${SMHA_LIB_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_sources(app PRIVATE
	src/main.c
	src/modem_reset.c
	src/modules/network/network.c
	src/modules/modem_at/modem_at.c
)

target_sources_ifdef(CONFIG_SHELL app PRIVATE src/modules/network/network_shell.c)
target_sources_ifdef(CONFIG_APP_CLOUD app PRIVATE src/modules/cloud/cloud.c)
target_sources_ifdef(CONFIG_APP_LOCATION app PRIVATE src/modules/location/location.c)
target_sources_ifdef(CONFIG_APP_MODEM_AT_SHELL app PRIVATE src/modules/modem_at/modem_at_shell.c)
target_sources_ifdef(CONFIG_APP_BUTTONS app PRIVATE src/modules/buttons/buttons.c)
target_sources_ifdef(CONFIG_APP_BUTTONS_SHELL app PRIVATE src/modules/buttons/buttons_shell.c)
