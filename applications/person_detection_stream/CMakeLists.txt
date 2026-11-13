#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project("Person detection video stream")

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
target_sources_ifdef(CONFIG_APP_NETWORK app PRIVATE src/modules/network/modem_baudrate.c)
target_sources_ifdef(CONFIG_APP_MODEM_AT app PRIVATE src/modules/modem_at/modem_at.c)
target_sources_ifdef(CONFIG_APP_MODEM_AT_SHELL app PRIVATE src/modules/modem_at/modem_at_shell.c)
target_sources_ifdef(CONFIG_APP_STREAM app PRIVATE src/modules/stream/stream.c)
target_sources_ifdef(CONFIG_APP_STREAM_SHELL app PRIVATE src/modules/stream/stream_shell.c)
target_sources_ifdef(CONFIG_APP_CAMERA app PRIVATE src/modules/camera/camera.c)
# Arducam Mega driver from the sdk-edge-ai person_detection application, with a JPEG quality
# control added; video_ctrls.h is private to Zephyr's video drivers.
if(CONFIG_APP_CAMERA)
	target_sources(app PRIVATE src/drivers/arducam_mega.c)
	target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/video)
endif()
target_sources_ifdef(CONFIG_APP_CAMERA_SHELL app PRIVATE src/modules/camera/camera_shell.c)
target_sources_ifdef(CONFIG_APP_DETECTION app PRIVATE src/modules/detection/detection.c)
target_sources_ifdef(CONFIG_APP_DETECTION_SHELL app PRIVATE src/modules/detection/detection_shell.c)

target_sources_ifdef(CONFIG_APP_FRAME_POOL app PRIVATE src/lib/frame_pool/frame_pool.c)

# Model and postprocessing come from the sdk-edge-ai person_detection application.
# Decoder speed bounds the detection rate, so JPEGDEC (built into jpeg.c) is built for speed.
if(CONFIG_APP_DETECTOR)
	set(PERSON_DET_DIR ${ZEPHYR_EDGE_AI_MODULE_DIR}/applications/person_detection/src)
	target_sources(app PRIVATE
		src/lib/detector/detector.c
		src/lib/jpeg/jpeg.c
		${PERSON_DET_DIR}/postprocessing.c
	)
	target_include_directories(app PRIVATE ${PERSON_DET_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/lib/jpegdec)
	set_source_files_properties(src/lib/jpeg/jpeg.c PROPERTIES
		COMPILE_OPTIONS "-O2;-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-pointer-to-int-cast")
endif()
