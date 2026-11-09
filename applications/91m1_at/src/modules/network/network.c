/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "modules/modem_at/modem_at.h"
#include "modules/modem_power/modem_power.h"
#include "network.h"

LOG_MODULE_REGISTER(network, CONFIG_APP_NETWORK_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS >
	     (CONFIG_APP_NETWORK_MODEM_READY_TIMEOUT_SECONDS +
	      CONFIG_APP_MODEM_AT_WAIT_TIMEOUT_SECONDS + CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS),
	     "Message processing timeout must cover the modem start-up and one AT command");

/* +CEREG <stat> values for a home or roaming registration. */
#define CEREG_REGISTERED_HOME		1
#define CEREG_REGISTERED_ROAMING	5

ZBUS_CHAN_DEFINE(network_chan,
		 struct network_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = NETWORK_DISCONNECTED));

ZBUS_MSG_SUBSCRIBER_DEFINE(network);

ZBUS_CHAN_ADD_OBS(network_chan, network, 0);

enum network_state {
	STATE_DISCONNECTED,
	STATE_CONNECTED,
};

struct network_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[sizeof(struct network_msg)];
};

static void publish_network_status(enum network_msg_type type);
static void on_cereg(char **argv, uint16_t argc, void *user_data);
static void disconnected_entry(void *obj);
static enum smf_state_result disconnected_run(void *obj);
static void connected_entry(void *obj);
static enum smf_state_result connected_run(void *obj);
static void network_wdt_callback(int channel_id, void *user_data);
static int modem_configure(void);

static const struct smf_state states[] = {
	[STATE_DISCONNECTED] = SMF_CREATE_STATE(disconnected_entry,
						disconnected_run, NULL, NULL, NULL),
	[STATE_CONNECTED] = SMF_CREATE_STATE(connected_entry,
					     connected_run, NULL, NULL, NULL),
};

static void publish_network_status(enum network_msg_type type)
{
	struct network_msg msg = { .type = type };
	int err;

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		FATAL_ERROR();
	}
}

/*
 * The +CEREG=5 URC is "+CEREG: <stat>[,"<tac>",...]". The read response
 * "+CEREG: <n>,<stat>,..." has the same prefix; its second argument is an unquoted
 * number, while the URC's second argument, if any, is the quoted tracking area code.
 */
static void on_cereg(char **argv, uint16_t argc, void *user_data)
{
	ARG_UNUSED(user_data);

	static bool registered;
	const char *stat_arg = argv[1];
	bool now_registered;
	int stat;

	if (argc < 2) {
		return;
	}

	if (argc >= 3 && argv[2][0] != '\0' && argv[2][0] != '"') {
		stat_arg = argv[2];
	}

	stat = atoi(stat_arg);
	now_registered = (stat == CEREG_REGISTERED_HOME) || (stat == CEREG_REGISTERED_ROAMING);

	LOG_DBG("LTE registration status: %d", stat);

	if (now_registered == registered) {
		return;
	}

	registered = now_registered;
	publish_network_status(registered ? NETWORK_CONNECTED : NETWORK_DISCONNECTED);
}

static void disconnected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
	LOG_DBG("Network module disconnected");
}

static enum smf_state_result disconnected_run(void *obj)
{
	int err;
	struct network_state_object *state_object = obj;
	const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

	switch (msg->type) {
	case NETWORK_CONNECT:
		LOG_DBG("Connect requested");

		err = modem_at_run("AT+CFUN=1", NULL, 0, CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS);
		if (err) {
			LOG_ERR("AT+CFUN=1, error: %d", err);
		}

		break;
	case NETWORK_CONNECTED:
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);
		break;
	default:
		break;
	}

	return SMF_EVENT_HANDLED;
}

static void connected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
	LOG_INF("Network connected");

#if IS_ENABLED(CONFIG_APP_MODEM_POWER)
	modem_power_saving_configure();
#endif
}

static enum smf_state_result connected_run(void *obj)
{
	int err;
	struct network_state_object *state_object = obj;
	const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

	switch (msg->type) {
	case NETWORK_DISCONNECT:
		LOG_DBG("Disconnect requested");

		err = modem_at_run("AT+CFUN=4", NULL, 0, CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS);
		if (err) {
			LOG_ERR("AT+CFUN=4, error: %d", err);
		}

		break;
	case NETWORK_DISCONNECTED:
		LOG_INF("Network disconnected");
		smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);
		break;
	default:
		break;
	}

	return SMF_EVENT_HANDLED;
}

static void network_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Network watchdog expired, channel: %d, thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* Wait for Serial Modem to answer after the host has reset it, then configure LTE.
 * Settings that need the modem offline are applied before the first AT+CFUN=1.
 */
static int modem_configure(void)
{
	static const char *const init_cmds[] = {
		"AT+CMEE=1",
		"AT+CEREG=5",
		/* LTE-M, NB-IoT and GNSS, no preference. */
		"AT%XSYSTEMMODE=1,1,1,0",
	};
	int64_t deadline = k_uptime_get() +
			   (CONFIG_APP_NETWORK_MODEM_READY_TIMEOUT_SECONDS * MSEC_PER_SEC);
	int err;

	do {
		err = modem_at_run("AT", NULL, 0, 1);
	} while (err && k_uptime_get() < deadline);

	if (err) {
		LOG_ERR("Serial Modem did not respond within %d s",
			CONFIG_APP_NETWORK_MODEM_READY_TIMEOUT_SECONDS);
		return err;
	}

	for (int i = 0; i < ARRAY_SIZE(init_cmds); i++) {
		err = modem_at_run(init_cmds[i], NULL, 0, CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS);
		if (err) {
			LOG_ERR("%s, error: %d", init_cmds[i], err);
			return err;
		}
	}

	LOG_INF("Serial Modem ready");

	return 0;
}

static void network_module(void)
{
	int err;
	int task_wdt_id;
	static struct network_state_object network_state;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait = K_MSEC(wdt_timeout_ms - execution_time_ms);

	task_wdt_id = task_wdt_add(wdt_timeout_ms, network_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		FATAL_ERROR();
	}

	err = modem_at_urc_subscribe("+CEREG: ", on_cereg, NULL);
	if (err) {
		LOG_ERR("Failed to subscribe to +CEREG URC: %d", err);
		FATAL_ERROR();
	}

	err = modem_configure();
	if (err) {
		FATAL_ERROR();
	}

	smf_set_initial(SMF_CTX(&network_state), &states[STATE_DISCONNECTED]);

#if IS_ENABLED(CONFIG_APP_NETWORK_SEARCH_NETWORK_ON_STARTUP)
	struct network_msg connect_msg = { .type = NETWORK_CONNECT };

	err = zbus_chan_pub(&network_chan, &connect_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		FATAL_ERROR();
	}
#endif

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			FATAL_ERROR();
		}

		err = zbus_sub_wait_msg(&network, &network_state.chan,
					network_state.msg_buf, zbus_wait);
		if (err == -ENOMSG) {
			continue;
		}
		if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			FATAL_ERROR();
		}

		err = smf_run_state(SMF_CTX(&network_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			FATAL_ERROR();
		}
	}
}

K_THREAD_DEFINE(network_module_thread, CONFIG_APP_NETWORK_THREAD_STACK_SIZE,
		network_module, NULL, NULL, NULL, 3, 0, 0);
