/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef APP_BUTTONS_H_
#define APP_BUTTONS_H_

#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

enum button_msg_type {
	/* Output: button 1 was pressed. */
	BUTTON_1 = 1,
	/* Output: button 2 was pressed. */
	BUTTON_2,
};

struct button_msg {
	enum button_msg_type type;
};

ZBUS_CHAN_DECLARE(button_chan);

#ifdef __cplusplus
}
#endif

#endif /* APP_BUTTONS_H_ */
