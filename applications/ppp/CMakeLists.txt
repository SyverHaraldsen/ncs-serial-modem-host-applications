#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

# Bundled nrf91-sm driver (see drivers/nrf91-sm/).
set(NRF91SM_DRIVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/drivers/nrf91-sm)
list(APPEND EXTRA_ZEPHYR_MODULES ${NRF91SM_DRIVER_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project("Serial Modem Host - PPP")

# Memfault CoAP includes unused nRF91 modem headers. Provide stubs on host MCU builds.
if(CONFIG_MEMFAULT_USE_NRF_CLOUD_COAP AND NOT CONFIG_NRF_MODEM_LIB)
	set(MEMFAULT_MODEM_SHIM ${CMAKE_CURRENT_SOURCE_DIR}/cmake/memfault-modem-shim)
	set(MEMFAULT_LIB_TARGET ..__modules__lib__memfault-firmware-sdk__ports__zephyr)
	if(TARGET ${MEMFAULT_LIB_TARGET})
		target_include_directories(${MEMFAULT_LIB_TARGET} BEFORE PRIVATE ${MEMFAULT_MODEM_SHIM})
	endif()
endif()

set(SMHA_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)

zephyr_include_directories(
	${SMHA_LIB_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_sources(app PRIVATE
	src/main.c
	src/modules/network/network.c
)
target_sources_ifdef(CONFIG_SHELL app PRIVATE src/modules/network/network_shell.c)
target_sources_ifdef(CONFIG_APP_CLOUD app PRIVATE src/modules/cloud/cloud.c)
target_sources_ifdef(CONFIG_APP_FOTA app PRIVATE src/modules/fota/fota.c)
target_sources_ifdef(CONFIG_APP_FOTA_SHELL app PRIVATE src/modules/fota/fota_shell.c)
target_sources_ifdef(CONFIG_APP_MODEM_AT app PRIVATE src/modules/modem_at/modem_at.c)
target_sources_ifdef(CONFIG_APP_MODEM_POWER app PRIVATE src/modules/modem_power/modem_power.c)
target_sources_ifdef(CONFIG_APP_MODEM_AT_SHELL app PRIVATE src/modules/modem_at/modem_at_shell.c)
target_sources_ifdef(CONFIG_APP_BUTTONS app PRIVATE src/modules/buttons/buttons.c)
target_sources_ifdef(CONFIG_APP_BUTTONS_SHELL app PRIVATE src/modules/buttons/buttons_shell.c)
target_sources_ifdef(CONFIG_APP_LED app PRIVATE src/modules/led/led.c)
target_sources_ifdef(CONFIG_APP_LOCATION_SHELL app PRIVATE
	src/modules/location/location_shell.c
)

# Only the nRF91 Serial Modem is reset by the host. The nRF93M1 DK modem node also
# has mdm-reset-gpios, but its in-tree driver owns that line.
target_sources_ifdef(CONFIG_DT_HAS_NORDIC_NRF91_SM_V2_ENABLED app PRIVATE src/modem_reset.c)

target_sources_ifdef(CONFIG_APP_BATTERY app PRIVATE src/modules/battery/battery.c)
target_sources_ifdef(CONFIG_APP_LOCATION app PRIVATE src/modules/cloud/cloud_location.c)
target_sources_ifdef(CONFIG_APP_LOCATION_BACKEND_LOCATION_LIB app PRIVATE
	src/modules/location/location_lib.c
)
target_sources_ifdef(CONFIG_APP_LOCATION_BACKEND_MODEM_AT app PRIVATE
	src/modules/location/location_at.c
)

# Memfault SDK guards new features with the minimum NCS version 3.4.0, which is yet to be released.
# Remove when the sdk tags 3.4.0.
if(CONFIG_APP_BATTERY AND CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX
   AND "${NCS_VERSION_MAJOR}.${NCS_VERSION_MINOR}.${NCS_VERSION_PATCH}" VERSION_LESS 3.4.0)
	zephyr_ld_options(-Wl,--wrap=nrf_fuel_gauge_init)
endif()
