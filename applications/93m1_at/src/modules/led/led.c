/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "led.h"

LOG_MODULE_REGISTER(led, CONFIG_APP_LED_LOG_LEVEL);

enum led_color {
	LED_COLOR_RED,
	LED_COLOR_GREEN,
	LED_COLOR_BLUE,
	LED_COLOR_COUNT,
};

static const struct gpio_dt_spec leds[LED_COLOR_COUNT] = {
	[LED_COLOR_RED] = GPIO_DT_SPEC_GET(DT_ALIAS(led_red), gpios),
	[LED_COLOR_GREEN] = GPIO_DT_SPEC_GET(DT_ALIAS(led_green), gpios),
	[LED_COLOR_BLUE] = GPIO_DT_SPEC_GET(DT_ALIAS(led_blue), gpios),
};

static void led_callback(const struct zbus_channel *chan);

ZBUS_LISTENER_DEFINE(led, led_callback);

ZBUS_CHAN_DEFINE(led_chan,
		 struct led_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_CHAN_ADD_OBS(led_chan, led, 0);

struct led_state {
	struct led_msg current;
	bool is_on;
	int repetitions;
};

static struct led_state led_state;
static struct k_work_delayable blink_work;

static int rgb_out(const struct led_msg *msg, bool on)
{
	const bool values[LED_COLOR_COUNT] = {
		[LED_COLOR_RED] = on && msg->red,
		[LED_COLOR_GREEN] = on && msg->green,
		[LED_COLOR_BLUE] = on && msg->blue,
	};

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		int err = gpio_pin_set_dt(&leds[i], values[i]);

		if (err) {
			LOG_ERR("gpio_pin_set_dt, error: %d", err);
			return err;
		}
	}

	return 0;
}

static void toggle_schedule(void)
{
	int err;
	uint32_t delay_msec = led_state.is_on ?
			      led_state.current.duration_on_msec :
			      led_state.current.duration_off_msec;

	err = k_work_schedule(&blink_work, K_MSEC(delay_msec));
	if (err < 0) {
		LOG_ERR("k_work_schedule, error: %d", err);
		FATAL_ERROR();
	}
}

static void blink_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	led_state.is_on = !led_state.is_on;

	err = rgb_out(&led_state.current, led_state.is_on);
	if (err) {
		LOG_ERR("rgb_out, error: %d", err);
		FATAL_ERROR();
		return;
	}

	/* Turning off completes one on/off cycle. */
	if (!led_state.is_on && led_state.repetitions > 0) {
		led_state.repetitions--;

		if (led_state.repetitions == 0) {
			return;
		}
	}

	toggle_schedule();
}

static void led_callback(const struct zbus_channel *chan)
{
	int err;
	const struct led_msg *msg;

	if (chan != &led_chan) {
		return;
	}

	msg = zbus_chan_const_msg(chan);

	if (msg->type != LED_RGB_SET) {
		return;
	}

	(void)k_work_cancel_delayable(&blink_work);

	led_state.current = *msg;
	led_state.repetitions = msg->repetitions;
	led_state.is_on = (msg->repetitions != 0);

	err = rgb_out(msg, led_state.is_on);
	if (err) {
		LOG_ERR("rgb_out, error: %d", err);
		FATAL_ERROR();
		return;
	}

	if (led_state.is_on && msg->duration_off_msec > 0) {
		toggle_schedule();
	}
}

static int led_init(void)
{
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (!gpio_is_ready_dt(&leds[i])) {
			LOG_ERR("LED GPIO %s is not ready", leds[i].port->name);
			FATAL_ERROR();
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
		if (err) {
			LOG_ERR("gpio_pin_configure_dt, error: %d", err);
			FATAL_ERROR();
			return err;
		}
	}

	k_work_init_delayable(&blink_work, blink_work_handler);

	return 0;
}

SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
