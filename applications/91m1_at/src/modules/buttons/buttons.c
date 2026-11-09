/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <dk_buttons_and_leds.h>

#include "app_common.h"
#include "buttons.h"

LOG_MODULE_REGISTER(buttons, CONFIG_APP_BUTTONS_LOG_LEVEL);

ZBUS_CHAN_DEFINE(button_chan,
		 struct button_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

static void publish(enum button_msg_type type)
{
	struct button_msg msg = { .type = type };
	int err;

	LOG_DBG("Button %u pressed", type);

	err = zbus_chan_pub(&button_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub button_chan, error: %d", err);
		FATAL_ERROR();
	}
}

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	uint32_t pressed = button_state & has_changed;

	if (pressed & DK_BTN1_MSK) {
		publish(BUTTON_1);
	}

	if (pressed & DK_BTN2_MSK) {
		publish(BUTTON_2);
	}
}

static int buttons_init(void)
{
	int err = dk_buttons_init(button_handler);

	if (err) {
		LOG_ERR("dk_buttons_init, error: %d", err);
		FATAL_ERROR();
	}

	return err;
}

SYS_INIT(buttons_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
