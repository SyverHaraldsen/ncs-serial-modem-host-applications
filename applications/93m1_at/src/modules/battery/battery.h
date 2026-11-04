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
	 * Input: read the charger and feed the nRF Fuel Gauge a new sample.
	 *
	 * The module publishes this to itself every CONFIG_APP_BATTERY_SAMPLE_INTERVAL seconds,
	 * starting when the thread has set up the fuel gauge.
	 */
	BATTERY_SAMPLE,
};

/** @brief Message published on @ref battery_chan. */
struct battery_msg {
	enum battery_msg_type type;
};

/**
 * @brief Battery module channel.
 *
 * Carries @ref BATTERY_SAMPLE to the module. The module publishes nothing back.
 */
ZBUS_CHAN_DECLARE(battery_chan);

/**
 * @brief Get the state of charge from the most recent sample.
 *
 * @param percent Populated with the state of charge in percent on success.
 *
 * @retval 0        Valid value written to @p percent.
 * @retval -EINVAL  @p percent is NULL.
 * @retval -ENODATA No valid sample available.
 */
int battery_percent_get(int *percent);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_H_ */
