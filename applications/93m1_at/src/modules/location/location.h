/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef LOCATION_H_
#define LOCATION_H_

#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Message types on @ref location_chan. */
enum location_msg_type {
	/**
	 * Input: request a location fix from the modem nRF Cloud client with
	 * `AT%NRFCLOUDLOCATION`, using the method set by `CONFIG_APP_LOCATION_METHOD`.
	 *
	 * The command returns once nRF Cloud has accepted the request. The resolved position
	 * arrives later as a URC and is only logged, it is not published on the channel.
	 */
	LOCATION_FIX_REQUEST,
	/**
	 * Output: the module has finished handling a @ref LOCATION_FIX_REQUEST.
	 *
	 * Published once per request, whether the request was accepted, rejected or
	 * unauthorized. It does not carry the outcome, which is logged by the module.
	 */
	LOCATION_FIX_DONE,
};

/** @brief Message published on @ref location_chan. */
struct location_msg {
	enum location_msg_type type;
};

/**
 * @brief Location module channel.
 *
 * Carries @ref LOCATION_FIX_REQUEST to the module and @ref LOCATION_FIX_DONE back to its
 * observers. The module also receives its own @ref LOCATION_FIX_DONE and ignores it.
 */
ZBUS_CHAN_DECLARE(location_chan);

#ifdef __cplusplus
}
#endif

#endif /* LOCATION_H_ */
