/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef CAMERA_H_
#define CAMERA_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Channel carrying @ref camera_msg requests to the camera module. */
ZBUS_CHAN_DECLARE(camera_chan);

/** @brief Frame size and JPEG quality the camera captures with. */
enum camera_profile {
	/** CAMERA_PROFILE_SET only: follow CAMERA_PROFILE_HINT. */
	CAMERA_PROFILE_AUTO = 0,
	/** CONFIG_APP_CAMERA_WIDTH x HEIGHT, sized for the LTE-M uplink. */
	CAMERA_PROFILE_LOW,
	/** CONFIG_APP_CAMERA_HIGH_WIDTH x HEIGHT, when the board enables it. */
	CAMERA_PROFILE_HIGH,
};

/** @brief Message types handled by the camera module. */
enum camera_msg_type {
	/** Set and store the frame rate limit in @ref camera_msg.fps. */
	CAMERA_FPS_SET = 0x1,

	/** Start or stop streaming the camera frames, see @ref camera_msg.stream. */
	CAMERA_STREAM_SET,

	/** Log the current status. */
	CAMERA_STATUS,

	/** Set and store the profile in @ref camera_msg.profile; only LOW overrides the hint. */
	CAMERA_PROFILE_SET,

	/** The profile the network in use calls for, in @ref camera_msg.profile. */
	CAMERA_PROFILE_HINT,
};

/** @brief Message carried on @ref camera_chan. */
struct camera_msg {
	enum camera_msg_type type;

	union {
		/** CAMERA_FPS_SET: frames per second, 0 for no limit. */
		uint8_t fps;

		/** CAMERA_STREAM_SET: false keeps capturing for detection without streaming. */
		bool stream;

		/** CAMERA_PROFILE_SET and CAMERA_PROFILE_HINT, see @ref camera_profile. */
		uint8_t profile;
	};
};

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H_ */
