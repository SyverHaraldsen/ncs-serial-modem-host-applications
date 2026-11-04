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

ZBUS_CHAN_DECLARE(cloud_chan);

enum cloud_msg_type {
	/* Battery state-of-charge percentage, value in .battery_percent. */
	CLOUD_BATTERY_SAMPLE = 0x1,
	/* Output: the battery report has finished, whether or not it succeeded. */
	CLOUD_BATTERY_DONE,
};

struct cloud_msg {
	enum cloud_msg_type type;
	int battery_percent;
};

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_H_ */
