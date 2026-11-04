/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "app_common.h"
#include "modules/modem_at/modem_at.h"
#include "cloud.h"

LOG_MODULE_REGISTER(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must exceed the maximum message processing time");
BUILD_ASSERT(CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS >
	     (CONFIG_APP_MODEM_AT_PIPE_WAIT_TIMEOUT_SECONDS +
	      CONFIG_APP_CLOUD_AT_TIMEOUT_SECONDS),
	     "Message processing timeout must exceed the AT pipe wait plus the AT command timeout");

ZBUS_CHAN_DEFINE(cloud_chan,
		 struct cloud_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(cloud);

#define CHANNEL_LIST(X)				\
	X(cloud_chan, struct cloud_msg)

#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, cloud, 0);

CHANNEL_LIST(ADD_OBSERVERS)

/* Helper functions */

static int cloud_send_battery(int percent)
{
	char cmd[CONFIG_APP_CLOUD_PAYLOAD_BUFFER];
	int len = sizeof(cmd);
	uint16_t coap_code;
	int err;

	err = snprintk(cmd, len,
		       "AT%%NRFCLOUDMESSAGE="
		       "{\"appId\":\"BATTERY\",\"messageType\":\"DATA\",\"data\":\"%d\"}", percent);
	if ((err < 0) || (err >= len)) {
		LOG_ERR("snprintk, error: %d", err);
		return -EINVAL;
	}

	LOG_DBG("Sending battery percentage: %d%%", percent);

	err = modem_at_run_coap(cmd, NULL, 0, CONFIG_APP_CLOUD_AT_TIMEOUT_SECONDS, &coap_code);
	if (err) {
		if (coap_code == MODEM_AT_COAP_UNAUTHORIZED) {
			return -EACCES;
		}
		LOG_ERR("modem_at_run_coap, error: %d, CoAP code: %u", err, coap_code);
		return -ENETUNREACH;
	}

	LOG_DBG("Battery percentage reported: %d%%", percent);

	return 0;
}

static void cloud_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Cloud watchdog expired, channel: %d, thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void cloud_thread(void)
{
	int err;
	int task_wdt_id;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait = K_MSEC(wdt_timeout_ms - execution_time_ms);

	task_wdt_id = task_wdt_add(wdt_timeout_ms, cloud_wdt_callback, (void *)k_current_get());
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

		err = zbus_sub_wait_msg(&cloud, &chan, msg_buf, zbus_wait);
		if (err == -ENOMSG) {
			continue;
		}
		if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			FATAL_ERROR();
		}

		if (chan == &cloud_chan) {
			const struct cloud_msg *msg = (const struct cloud_msg *)msg_buf;

			if (msg->type == CLOUD_BATTERY_SAMPLE) {
				LOG_DBG("Battery sample received: %d%%", msg->battery_percent);
				err = cloud_send_battery(msg->battery_percent);
				if (err == -EACCES) {
					LOG_WRN("Failed to send battery data, unauthorized "
						"(device may not be onboarded to nRF Cloud)");
				} else if (err == -ENETUNREACH) {
					LOG_WRN("Failed to send battery data, request rejected");
				} else if (err) {
					LOG_ERR("cloud_send_battery, error: %d", err);
					FATAL_ERROR();
				} else {
				}

				struct cloud_msg msg_done = { .type = CLOUD_BATTERY_DONE };

				err = zbus_chan_pub(&cloud_chan, &msg_done, PUB_TIMEOUT);
				if (err) {
					LOG_ERR("zbus_chan_pub cloud_chan, error: %d", err);
					FATAL_ERROR();
				}
			}
		} else {
			LOG_WRN("Unhandled message in cloud thread");
		}
	}
}

K_THREAD_DEFINE(cloud_tid, CONFIG_APP_CLOUD_THREAD_STACK_SIZE, cloud_thread,
		NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
