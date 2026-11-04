/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef BATTERY_H_
#define BATTERY_H_

#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Message types on @ref battery_chan. */
enum battery_msg_type {
	/**
	 * Input: request the state of charge from the most recent fuel gauge sample.
	 *
	 * Answered with @ref BATTERY_PERCENT_RESPONSE, or with
	 * @ref BATTERY_PERCENT_UNAVAILABLE when there is no valid sample.
	 */
	BATTERY_PERCENT_REQUEST = 0x1,
	/** Output: the state of charge in percent, in @ref battery_msg.percent. */
	BATTERY_PERCENT_RESPONSE,
	/** Output: the fuel gauge has no valid sample to answer a request with. */
	BATTERY_PERCENT_UNAVAILABLE,
};

/** @brief Message published on @ref battery_chan. */
struct battery_msg {
	enum battery_msg_type type;
	/** State of charge in percent. Only used by @ref BATTERY_PERCENT_RESPONSE. */
	int percent;
};

/**
 * @brief Battery module channel.
 *
 * Carries @ref BATTERY_PERCENT_REQUEST to the module and its answer back to the observers.
 * The module also receives its own answers and ignores them.
 *
 * The module samples the fuel gauge every CONFIG_APP_BATTERY_SAMPLE_INTERVAL seconds on its
 * own, so a request only reads the latest sample and does not trigger a new one.
 */
ZBUS_CHAN_DECLARE(battery_chan);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_H_ */
