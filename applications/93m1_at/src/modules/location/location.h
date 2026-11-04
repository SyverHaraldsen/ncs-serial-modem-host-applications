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

enum location_msg_type {
	/* Input: request a location fix. */
	LOCATION_FIX_REQUEST,
	/* Output: the location request has finished, whether or not it succeeded. */
	LOCATION_FIX_DONE,
};

struct location_msg {
	enum location_msg_type type;
};

ZBUS_CHAN_DECLARE(location_chan);

#ifdef __cplusplus
}
#endif

#endif /* LOCATION_H_ */
