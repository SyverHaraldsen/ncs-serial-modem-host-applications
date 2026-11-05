/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "modules/modem_at/modem_at.h"
#include "modules/modem_power/modem_power.h"
#include "network.h"

LOG_MODULE_REGISTER(network, CONFIG_APP_NETWORK_LOG_LEVEL);

#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

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
static void l4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			     struct net_if *iface);
static void connectivity_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
				       struct net_if *iface);
static void disconnected_entry(void *obj);
static enum smf_state_result disconnected_run(void *obj);
static void connected_entry(void *obj);
static enum smf_state_result connected_run(void *obj);
static void network_wdt_callback(int channel_id, void *user_data);
static void log_device_uuid(void);

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

static void l4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			     struct net_if *iface)
{
	ARG_UNUSED(cb);

	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		LOG_DBG("Network connectivity established (iface %d)", net_if_get_by_iface(iface));

		publish_network_status(NETWORK_CONNECTED);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_DBG("Network connectivity lost (iface %d)", net_if_get_by_iface(iface));

		publish_network_status(NETWORK_DISCONNECTED);
		break;
	default:
		break;
	}
}

static void connectivity_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
				       struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_CONN_IF_FATAL_ERROR) {
		LOG_ERR("NET_EVENT_CONN_IF_FATAL_ERROR");
		FATAL_ERROR();
	}
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

		err = conn_mgr_all_if_up(true);
		if (err) {
			LOG_ERR("conn_mgr_all_if_up, error: %d", err);
			FATAL_ERROR();
		}

		err = conn_mgr_all_if_connect(true);
		if (err) {
			LOG_ERR("conn_mgr_all_if_connect, error: %d", err);
			FATAL_ERROR();
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
	LOG_DBG("Network module connected");

#if IS_ENABLED(CONFIG_APP_MODEM_POWER)
	modem_power_saving_configure();
#endif

	log_device_uuid();
}

static enum smf_state_result connected_run(void *obj)
{
	int err;
	struct network_state_object *state_object = obj;
	const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

	switch (msg->type) {
	case NETWORK_DISCONNECT:
		LOG_DBG("Disconnect requested");

		err = conn_mgr_all_if_down(true);
		if (err) {
			LOG_ERR("conn_mgr_all_if_down, error: %d", err);
			FATAL_ERROR();
		}

		break;
	case NETWORK_DISCONNECTED:
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

static void log_device_uuid(void)
{
	static bool logged;
	static const char prefix[] = "%DEVICEUUID: ";
	char resp[64];
	char *uuid = resp;
	int err;

	if (logged) {
		return;
	}

	err = modem_at_run("AT%DEVICEUUID", resp, sizeof(resp), 10);
	if (err) {
		LOG_WRN("Failed to read the device UUID, error: %d", err);
		return;
	}

	resp[strcspn(resp, "\r\n")] = '\0';
	if (strncmp(uuid, prefix, sizeof(prefix) - 1) == 0) {
		uuid += sizeof(prefix) - 1;
	}

	LOG_INF("Device UUID: %s", uuid);
	logged = true;
}

static void network_module(void)
{
	int err;
	int task_wdt_id;
	static struct net_mgmt_event_callback l4_cb;
	static struct net_mgmt_event_callback conn_cb;
	static struct network_state_object network_state;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait = K_MSEC(wdt_timeout_ms - execution_time_ms);

	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);

	net_mgmt_init_event_callback(&conn_cb, connectivity_event_handler, CONN_LAYER_EVENT_MASK);
	net_mgmt_add_event_callback(&conn_cb);

	task_wdt_id = task_wdt_add(wdt_timeout_ms, network_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
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
