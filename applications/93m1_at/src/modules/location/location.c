/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "app_common.h"
#include "modules/modem_at/modem_at.h"
#include "location.h"

LOG_MODULE_REGISTER(location, CONFIG_APP_LOCATION_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_LOCATION_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must exceed the maximum message processing time");
BUILD_ASSERT(CONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS >
	     (CONFIG_APP_MODEM_AT_PIPE_WAIT_TIMEOUT_SECONDS +
	      CONFIG_APP_LOCATION_AT_TIMEOUT_SECONDS),
	     "Message processing timeout must exceed the AT pipe wait plus the AT command timeout");

ZBUS_CHAN_DEFINE(location_chan,
		 struct location_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(location);

#define CHANNEL_LIST(X)				\
	X(location_chan, struct location_msg)

#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, location, 0);

CHANNEL_LIST(ADD_OBSERVERS)

/* Forward declarations */
static bool is_coordinate(const char *s);
static const char *location_method_str(const char *method_num);
static void on_location(char **argv, uint16_t argc, void *user_data);
static int location_request(void);
static void location_thread(void);

/* A fix line starts with a numeric latitude; anything else is a status URC. */
static bool is_coordinate(const char *s)
{
	return s != NULL && (*s == '-' || *s == '+' || (*s >= '0' && *s <= '9'));
}

static const char *location_method_str(const char *method_num)
{
	switch (atoi(method_num)) {
	case 1: return "Single-cell";
	case 2: return "Multicell";
	case 4: return "Wi-Fi";
	default: return method_num;
	}
}

/* %NRFCLOUDLOCATION: <lat>,<lon>,<unc>,<method> — async result URC. */
static void on_location(char **argv, uint16_t argc, void *user_data)
{
	ARG_UNUSED(user_data);

	if (argc >= 5 && is_coordinate(argv[1])) {
		LOG_DBG("Location: %s,%s Uncertainty: %sm Method: %s",
			argv[1], argv[2], argv[3], location_method_str(argv[4]));
		LOG_DBG("Google maps URL: https://maps.google.com/?q=%s,%s", argv[1], argv[2]);
	} else if (argc >= 2) {
		LOG_DBG("NRFCLOUDLOCATION status: %s", argv[1]);
	} else {
		LOG_WRN("NRFCLOUDLOCATION URC with unexpected format");
	}
}

static int location_request(void)
{
	char cmd[40];
	uint16_t coap_code;
	int err;
	int ret;

	ret = snprintk(cmd, sizeof(cmd), "AT%%NRFCLOUDLOCATION=%d,1", CONFIG_APP_LOCATION_METHOD);
	if (ret < 0 || ret >= (int)sizeof(cmd)) {
		LOG_ERR("snprintk, error: %d", ret);
		return -EINVAL;
	}

	LOG_DBG("Requesting location, method: %d", CONFIG_APP_LOCATION_METHOD);

	err = modem_at_run_coap(cmd, NULL, 0, CONFIG_APP_LOCATION_AT_TIMEOUT_SECONDS, &coap_code);
	if (err) {
		if (coap_code == MODEM_AT_COAP_UNAUTHORIZED) {
			return -EACCES;
		}
		LOG_ERR("modem_at_run_coap, error: %d, CoAP code: %u", err, coap_code);
		return -ENETUNREACH;
	}

	return 0;
}

static void location_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Location watchdog expired, channel: %d, thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void location_thread(void)
{
	int err;
	int task_wdt_id;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_LOCATION_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait = K_MSEC(wdt_timeout_ms - execution_time_ms);

	err = modem_at_urc_subscribe("%NRFCLOUDLOCATION: ", on_location, NULL);
	if (err) {
		LOG_ERR("Failed to subscribe to %%NRFCLOUDLOCATION URC: %d", err);
		FATAL_ERROR();
	}

	task_wdt_id = task_wdt_add(wdt_timeout_ms, location_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("task_wdt_add, error: %d", task_wdt_id);
		FATAL_ERROR();
	}

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			FATAL_ERROR();
		}

		err = zbus_sub_wait_msg(&location, &chan, msg_buf, zbus_wait);
		if (err == -ENOMSG) {
			continue;
		}
		if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			FATAL_ERROR();
		}

		if (chan == &location_chan) {
			const struct location_msg *msg = (const struct location_msg *)msg_buf;

			if (msg->type == LOCATION_FIX_REQUEST) {
				LOG_DBG("Location fix request received");
				err = location_request();
				if (err == -EACCES) {
					LOG_WRN("Failed to request location, unauthorized "
						"(device may not be onboarded to nRF Cloud)");
				} else if (err == -ENETUNREACH) {
					LOG_WRN("Failed to request location, request rejected");
				} else if (err) {
					LOG_ERR("location_request, error: %d", err);
					FATAL_ERROR();
				} else {
				}

				struct location_msg msg_done = { .type = LOCATION_FIX_DONE };

				err = zbus_chan_pub(&location_chan, &msg_done, PUB_TIMEOUT);
				if (err) {
					LOG_ERR("zbus_chan_pub location_chan, error: %d", err);
					FATAL_ERROR();
				}
			}
		} else {
			LOG_WRN("Unhandled message in location thread");
		}
	}
}

K_THREAD_DEFINE(location_tid, CONFIG_APP_LOCATION_THREAD_STACK_SIZE, location_thread,
		NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
