/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_l2.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/pm/device.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#if defined(CONFIG_APP_MODEM_AT)
#include "modules/modem_at/modem_at.h"
#endif
#include "network.h"

LOG_MODULE_REGISTER(network, CONFIG_APP_NETWORK_LOG_LEVEL);

#define IPV4_EVENT_MASK (NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL)
#define IF_EVENT_MASK (NET_EVENT_IF_UP | NET_EVENT_IF_DOWN)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

ZBUS_CHAN_DEFINE(network_chan,
		 struct network_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = NETWORK_IFACE_DOWN));

ZBUS_MSG_SUBSCRIBER_DEFINE(network);

ZBUS_CHAN_ADD_OBS(network_chan, network, 0);

enum network_state {
	STATE_RUNNING,
};

struct network_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[sizeof(struct network_msg)];
};

/* BIT(enum network_iface) per interface that can carry traffic. */
static atomic_t ifaces_up;

static enum smf_state_result running_run(void *obj);
static void network_wdt_callback(int channel_id, void *user_data);
static void log_device_uuid(void);

static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(NULL, running_run, NULL, NULL, NULL),
};

struct net_if *network_iface_get(enum network_iface iface)
{
	switch (iface) {
#if defined(CONFIG_WIFI)
	case NETWORK_IFACE_WIFI:
		return net_if_get_first_wifi();
#endif
#if defined(CONFIG_NET_L2_PPP)
	case NETWORK_IFACE_LTE:
		return net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
#endif
	default:
		return NULL;
	}
}

const char *network_iface_name(enum network_iface iface)
{
	static const char *const names[] = {
		[NETWORK_IFACE_NONE] = "none",
		[NETWORK_IFACE_WIFI] = "wifi",
		[NETWORK_IFACE_LTE] = "lte",
	};

	return iface < ARRAY_SIZE(names) ? names[iface] : "?";
}

static void publish(enum network_msg_type type, enum network_iface iface)
{
	struct network_msg msg = { .type = type, .iface = iface };
	int err;

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		FATAL_ERROR();
	}
}

static bool iface_ready(struct net_if *iface)
{
	return net_if_is_up(iface) &&
	       net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

/* Only called from the net_mgmt thread, so the read-modify-write cannot race. */
static void ifaces_evaluate(void)
{
	for (enum network_iface i = NETWORK_IFACE_WIFI; i < NETWORK_IFACE_COUNT; i++) {
		struct net_if *iface = network_iface_get(i);
		const bool ready = iface != NULL && iface_ready(iface);

		if (ready == atomic_test_bit(&ifaces_up, i)) {
			continue;
		}

		atomic_set_bit_to(&ifaces_up, i, ready);
		LOG_INF("%s %s", network_iface_name(i), ready ? "up" : "down");
		publish(ready ? NETWORK_IFACE_UP : NETWORK_IFACE_DOWN, i);
	}
}

static void iface_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
				struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(event);
	ARG_UNUSED(iface);

	ifaces_evaluate();
}

static void connectivity_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
				       struct net_if *iface)
{
	ARG_UNUSED(cb);

	/* One interface failing, such as Wi-Fi without stored credentials, must not take
	 * down the others.
	 */
	if (event == NET_EVENT_CONN_IF_FATAL_ERROR) {
		LOG_WRN("Interface %s gave up connecting", net_if_get_device(iface)->name);
	}
}

static void iface_request(enum network_iface i, bool up)
{
	struct net_if *iface = network_iface_get(i);
	int err;

	if (iface == NULL) {
		LOG_WRN("No %s interface", network_iface_name(i));
		return;
	}

	LOG_INF("Taking %s %s", network_iface_name(i), up ? "up" : "down");

	/* PPP has no connectivity binding; its admin state powers the modem. */
	if (conn_mgr_if_is_bound(iface)) {
		err = up ? conn_mgr_if_connect(iface) : conn_mgr_if_disconnect(iface);
	} else {
		err = up ? net_if_up(iface) : net_if_down(iface);
	}

	if (err && err != -EALREADY) {
		LOG_WRN("%s %s, error: %d", network_iface_name(i), up ? "up" : "down", err);
	}
}

#if defined(CONFIG_MODEM_CELLULAR)
/* modem_cellular wedges when PPP is toggled while it is still dialing, so one LTE
 * transition runs at a time.
 */
static bool lte_want;
static bool lte_busy;

static void lte_timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	publish(NETWORK_LTE_TIMEOUT, NETWORK_IFACE_LTE);
}

static K_WORK_DELAYABLE_DEFINE(lte_timeout_work, lte_timeout_handler);

static void lte_wait(void)
{
	lte_busy = true;
	k_work_reschedule(&lte_timeout_work, K_SECONDS(CONFIG_APP_NETWORK_LTE_RECOVERY_SECONDS));
}

static void lte_done(void)
{
	lte_busy = false;
	k_work_cancel_delayable(&lte_timeout_work);
}

static void lte_request(bool up)
{
	lte_want = up;

	if (lte_busy) {
		LOG_INF("LTE still coming up, taking it %s afterwards", up ? "up" : "down");
		return;
	}

	if (up && atomic_test_bit(&ifaces_up, NETWORK_IFACE_LTE)) {
		return;
	}

	iface_request(NETWORK_IFACE_LTE, up);

	if (up) {
		lte_wait();
	}
}

static void lte_settled(void)
{
	lte_done();

	if (!lte_want) {
		iface_request(NETWORK_IFACE_LTE, false);
	}
}

static void lte_power_cycle(void)
{
	const struct device *modem = DEVICE_DT_GET(DT_NODELABEL(modem));
	enum pm_device_state state;
	int err;

	if (!net_if_is_admin_up(network_iface_get(NETWORK_IFACE_LTE))) {
		lte_request(true);
		return;
	}

	LOG_WRN("LTE not up after %d s, power cycling the modem",
		CONFIG_APP_NETWORK_LTE_RECOVERY_SECONDS);

	/* Bypasses runtime PM, which still holds the reference PPP took. */
	err = pm_device_state_get(modem, &state);
	if (err == 0 && state == PM_DEVICE_STATE_ACTIVE) {
		err = pm_device_action_run(modem, PM_DEVICE_ACTION_SUSPEND);
		if (err) {
			LOG_WRN("Modem suspend, error: %d", err);
		}
	}

	err = pm_device_action_run(modem, PM_DEVICE_ACTION_RESUME);
	if (err) {
		LOG_WRN("Modem resume, error: %d", err);
	}

	lte_wait();
}

static void lte_timeout(void)
{
	if (!lte_busy || atomic_test_bit(&ifaces_up, NETWORK_IFACE_LTE)) {
		return;
	}

	if (lte_want) {
		lte_power_cycle();
	} else {
		lte_settled();
	}
}
#endif /* CONFIG_MODEM_CELLULAR */

static void iface_connect(enum network_iface i, bool up)
{
#if defined(CONFIG_MODEM_CELLULAR)
	if (i == NETWORK_IFACE_LTE) {
		lte_request(up);
		return;
	}
#endif
	iface_request(i, up);
}

static enum smf_state_result running_run(void *obj)
{
	int err;
	struct network_state_object *state_object = obj;
	const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

	switch (msg->type) {
	case NETWORK_CONNECT:
		err = conn_mgr_all_if_up(true);
		if (err) {
			LOG_WRN("conn_mgr_all_if_up, error: %d", err);
		}

		err = conn_mgr_all_if_connect(true);
		if (err) {
			LOG_WRN("conn_mgr_all_if_connect, error: %d", err);
		}
#if defined(CONFIG_MODEM_CELLULAR)
		lte_want = true;
		lte_wait();
#endif
		break;
	case NETWORK_DISCONNECT:
#if defined(CONFIG_MODEM_CELLULAR)
		lte_want = false;
		lte_done();
#endif
		err = conn_mgr_all_if_down(true);
		if (err) {
			LOG_WRN("conn_mgr_all_if_down, error: %d", err);
		}
		break;
	case NETWORK_IFACE_CONNECT:
		iface_connect(msg->iface, true);
		break;
	case NETWORK_IFACE_DISCONNECT:
		iface_connect(msg->iface, false);
		break;
	case NETWORK_IFACE_UP:
		if (msg->iface == NETWORK_IFACE_LTE) {
#if defined(CONFIG_MODEM_CELLULAR)
			lte_settled();
#endif
			log_device_uuid();
		}
		break;
#if defined(CONFIG_MODEM_CELLULAR)
	case NETWORK_LTE_TIMEOUT:
		lte_timeout();
		break;
#endif
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
#if defined(CONFIG_APP_MODEM_AT)
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
#endif
}

static void network_module(void)
{
	int err;
	int task_wdt_id;
	static struct net_mgmt_event_callback ipv4_cb;
	static struct net_mgmt_event_callback if_cb;
	static struct net_mgmt_event_callback conn_cb;
	static struct network_state_object network_state;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait = K_MSEC(wdt_timeout_ms - execution_time_ms);

	net_mgmt_init_event_callback(&ipv4_cb, iface_event_handler, IPV4_EVENT_MASK);
	net_mgmt_add_event_callback(&ipv4_cb);

	net_mgmt_init_event_callback(&if_cb, iface_event_handler, IF_EVENT_MASK);
	net_mgmt_add_event_callback(&if_cb);

	net_mgmt_init_event_callback(&conn_cb, connectivity_event_handler, CONN_LAYER_EVENT_MASK);
	net_mgmt_add_event_callback(&conn_cb);

	task_wdt_id = task_wdt_add(wdt_timeout_ms, network_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		FATAL_ERROR();
	}

	smf_set_initial(SMF_CTX(&network_state), &states[STATE_RUNNING]);

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
