/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef CLOUD_H_
#define CLOUD_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Cloud module channel.
 *
 * Carries @ref CLOUD_BATTERY_SAMPLE to the module and @ref CLOUD_BATTERY_DONE back to its
 * observers. The module also receives its own @ref CLOUD_BATTERY_DONE and ignores it.
 */
ZBUS_CHAN_DECLARE(cloud_chan);

/** @brief Message types on @ref cloud_chan. */
enum cloud_msg_type {
	/**
	 * Input: report the battery state of charge in @ref cloud_msg.battery_percent to nRF Cloud
	 * as a device message with `AT%NRFCLOUDMESSAGE`.
	 */
	CLOUD_BATTERY_SAMPLE = 0x1,
	/**
	 * Output: the module has finished handling a @ref CLOUD_BATTERY_SAMPLE.
	 *
	 * Published once per sample, whether the report was sent, rejected or unauthorized.
	 * It does not carry the outcome, which is logged by the module.
	 */
	CLOUD_BATTERY_DONE,
};

/** @brief Message published on @ref cloud_chan. */
struct cloud_msg {
	enum cloud_msg_type type;
	/** State of charge in percent. Only used by @ref CLOUD_BATTERY_SAMPLE. */
	int battery_percent;
};

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_H_ */
