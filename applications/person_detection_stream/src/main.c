/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "modules/network/network.h"
#include "modules/stream/stream.h"
#if defined(CONFIG_APP_CAMERA)
#include "modules/camera/camera.h"
#endif

LOG_MODULE_REGISTER(main, CONFIG_APP_MAIN_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_MAIN_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_MAIN_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Published by the switch timer; the id drops a timeout from an earlier switch. */
struct switch_timeout_msg {
	uint32_t id;
};

ZBUS_CHAN_DEFINE(switch_timeout_chan,
		 struct switch_timeout_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(main_subscriber);

#define CHANNEL_LIST(X) \
	X(network_chan, struct network_msg) \
	X(switch_timeout_chan, struct switch_timeout_msg)

#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, main_subscriber, 0);

CHANNEL_LIST(ADD_OBSERVERS)

enum main_app_state {
	/** Handles interface choice requests and status in every child state. */
	STATE_RUNNING,
	/** No usable interface. The stream is told to close its socket. */
	STATE_OFFLINE,
	/** The stream runs over @ref main_state.active. */
	STATE_ONLINE,
	/** Waiting for @ref main_state.target; the stream stays on the current interface. */
	STATE_SWITCHING,
};

struct main_state {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];

	/** Requested interface; NETWORK_IFACE_NONE chooses automatically. */
	enum network_iface mode;
	/** Mode to fall back to when a switch times out. */
	enum network_iface prev_mode;
	/** BIT(enum network_iface) per interface that is up. */
	uint32_t up;
	/** Interface carrying the stream, or NETWORK_IFACE_NONE. */
	enum network_iface active;
	/** STATE_SWITCHING only: the interface being brought up. */
	enum network_iface target;
	uint32_t switch_id;
};

static struct main_state main_state;

static void switch_timeout_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(switch_timeout_work, switch_timeout_work_fn);

static enum smf_state_result state_running_run(void *obj);
static void state_offline_entry(void *obj);
static enum smf_state_result state_offline_run(void *obj);
static void state_online_entry(void *obj);
static enum smf_state_result state_online_run(void *obj);
static void state_switching_entry(void *obj);
static enum smf_state_result state_switching_run(void *obj);
static void state_switching_exit(void *obj);

static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(NULL, state_running_run, NULL, NULL,
					   &states[STATE_OFFLINE]),
	[STATE_OFFLINE] = SMF_CREATE_STATE(state_offline_entry, state_offline_run, NULL,
					   &states[STATE_RUNNING], NULL),
	[STATE_ONLINE] = SMF_CREATE_STATE(state_online_entry, state_online_run, NULL,
					  &states[STATE_RUNNING], NULL),
	[STATE_SWITCHING] = SMF_CREATE_STATE(state_switching_entry, state_switching_run,
					     state_switching_exit, &states[STATE_RUNNING], NULL),
};

static void switch_timeout_work_fn(struct k_work *work)
{
	struct switch_timeout_msg msg = { .id = main_state.switch_id };

	ARG_UNUSED(work);

	(void)zbus_chan_pub(&switch_timeout_chan, &msg, PUB_TIMEOUT);
}

static void stream_request(enum stream_msg_type type, enum network_iface iface)
{
	struct stream_msg msg = { .type = type };
	struct net_if *net_if = network_iface_get(iface);
	int err;

	if (net_if != NULL) {
		msg.iface = net_if_get_by_iface(net_if);
	}

	err = zbus_chan_pub(&stream_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub stream_chan, error: %d", err);
		FATAL_ERROR();
	}
}

/* Wi-Fi has room for larger frames than the LTE-M uplink. */
static void camera_hint(enum network_iface iface)
{
#if defined(CONFIG_APP_CAMERA)
	struct camera_msg msg = {
		.type = CAMERA_PROFILE_HINT,
		.profile = iface == NETWORK_IFACE_WIFI ? CAMERA_PROFILE_HIGH : CAMERA_PROFILE_LOW,
	};
	int err;

	err = zbus_chan_pub(&camera_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub camera_chan, error: %d", err);
		FATAL_ERROR();
	}
#else
	ARG_UNUSED(iface);
#endif
}

static void network_request(enum network_msg_type type, enum network_iface iface)
{
	struct network_msg msg = { .type = type, .iface = iface };
	int err;

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub network_chan, error: %d", err);
		FATAL_ERROR();
	}
}

static bool is_up(const struct main_state *s, enum network_iface iface)
{
	return (s->up & BIT(iface)) != 0;
}

static enum network_iface other_iface(enum network_iface iface)
{
	return iface == NETWORK_IFACE_WIFI ? NETWORK_IFACE_LTE : NETWORK_IFACE_WIFI;
}

/* An interface the mode allows that is up, Wi-Fi first. */
static enum network_iface choose(const struct main_state *s)
{
	if (s->mode != NETWORK_IFACE_NONE) {
		return is_up(s, s->mode) ? s->mode : NETWORK_IFACE_NONE;
	}

	if (is_up(s, NETWORK_IFACE_WIFI)) {
		return NETWORK_IFACE_WIFI;
	}

	return is_up(s, NETWORK_IFACE_LTE) ? NETWORK_IFACE_LTE : NETWORK_IFACE_NONE;
}

static const struct network_msg *network_msg_get(const struct main_state *s)
{
	return s->chan == &network_chan ? (const struct network_msg *)s->msg_buf : NULL;
}

static void go_online(struct main_state *s, enum network_iface iface)
{
	s->active = iface;
	smf_set_state(SMF_CTX(s), &states[STATE_ONLINE]);
}

static void online_apply(struct main_state *s)
{
	LOG_INF("Streaming over %s", network_iface_name(s->active));
	camera_hint(s->active);
	stream_request(STREAM_CONNECT, s->active);

	/* Auto mode keeps Wi-Fi trying while on LTE, but cellular goes down under Wi-Fi. */
	if ((s->mode != NETWORK_IFACE_NONE || s->active == NETWORK_IFACE_WIFI) &&
	    network_iface_get(other_iface(s->active)) != NULL) {
		network_request(NETWORK_IFACE_DISCONNECT, other_iface(s->active));
	}
}

static void status_log(const struct main_state *s)
{
	LOG_INF("Mode %s, streaming over %s; wifi %s, lte %s",
		s->mode == NETWORK_IFACE_NONE ? "auto" : network_iface_name(s->mode),
		network_iface_name(s->active),
		is_up(s, NETWORK_IFACE_WIFI) ? "up" : "down",
		is_up(s, NETWORK_IFACE_LTE) ? "up" : "down");
}

static void use_request(struct main_state *s, enum network_iface target)
{
	if (target != NETWORK_IFACE_NONE && network_iface_get(target) == NULL) {
		LOG_WRN("No %s interface on this board", network_iface_name(target));
		return;
	}

	s->prev_mode = s->mode;
	s->mode = target;

	if (target == NETWORK_IFACE_NONE) {
		LOG_INF("Choosing the interface automatically");

		/* Reconnecting Wi-Fi drops its association, so only connect what is down. */
		for (enum network_iface i = NETWORK_IFACE_WIFI; i < NETWORK_IFACE_COUNT; i++) {
			if (network_iface_get(i) != NULL && !is_up(s, i)) {
				network_request(NETWORK_IFACE_CONNECT, i);
			}
		}

		if (choose(s) != NETWORK_IFACE_NONE && choose(s) != s->active) {
			go_online(s, choose(s));
		}
		return;
	}

	if (is_up(s, target)) {
		go_online(s, target);
	} else if (s->active == NETWORK_IFACE_NONE) {
		smf_set_state(SMF_CTX(s), &states[STATE_OFFLINE]);
	} else {
		s->target = target;
		smf_set_state(SMF_CTX(s), &states[STATE_SWITCHING]);
	}
}

static enum smf_state_result state_running_run(void *obj)
{
	struct main_state *s = obj;
	const struct network_msg *msg = network_msg_get(s);

	if (msg == NULL) {
		return SMF_EVENT_HANDLED;
	}

	switch (msg->type) {
	case NETWORK_USE:
		use_request(s, msg->iface);
		break;
	case NETWORK_STATUS:
		status_log(s);
		break;
	default:
		break;
	}

	return SMF_EVENT_HANDLED;
}

static void state_offline_entry(void *obj)
{
	struct main_state *s = obj;

	LOG_INF("Network disconnected");

	s->active = NETWORK_IFACE_NONE;
	stream_request(STREAM_DISCONNECT, NETWORK_IFACE_NONE);

	/* In auto mode LTE may have been taken down under Wi-Fi; it is the fallback now. */
	const enum network_iface fallback =
		s->mode == NETWORK_IFACE_NONE ? NETWORK_IFACE_LTE : s->mode;

	if (network_iface_get(fallback) != NULL) {
		network_request(NETWORK_IFACE_CONNECT, fallback);
	}
}

static enum smf_state_result state_offline_run(void *obj)
{
	struct main_state *s = obj;
	const struct network_msg *msg = network_msg_get(s);

	if (msg != NULL && msg->type == NETWORK_IFACE_UP && choose(s) != NETWORK_IFACE_NONE) {
		go_online(s, choose(s));
		return SMF_EVENT_HANDLED;
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_online_entry(void *obj)
{
	LOG_INF("Network connected");

	online_apply(obj);
}

static enum smf_state_result state_online_run(void *obj)
{
	struct main_state *s = obj;
	const struct network_msg *msg = network_msg_get(s);

	if (msg == NULL) {
		return SMF_EVENT_PROPAGATE;
	}

	if (msg->type == NETWORK_IFACE_DOWN && msg->iface == s->active) {
		if (choose(s) != NETWORK_IFACE_NONE) {
			s->active = choose(s);
			online_apply(s);
		} else {
			smf_set_state(SMF_CTX(s), &states[STATE_OFFLINE]);
		}
		return SMF_EVENT_HANDLED;
	}

	if (msg->type == NETWORK_IFACE_UP && s->mode == NETWORK_IFACE_NONE &&
	    msg->iface == NETWORK_IFACE_WIFI && s->active != NETWORK_IFACE_WIFI) {
		s->active = NETWORK_IFACE_WIFI;
		online_apply(s);
		return SMF_EVENT_HANDLED;
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_switching_entry(void *obj)
{
	struct main_state *s = obj;

	LOG_INF("Switching to %s, streaming over %s until it is up",
		network_iface_name(s->target), network_iface_name(s->active));

	s->switch_id++;
	network_request(NETWORK_IFACE_CONNECT, s->target);
	(void)k_work_reschedule(&switch_timeout_work,
				K_SECONDS(CONFIG_APP_MAIN_SWITCH_TIMEOUT_SECONDS));
}

static enum smf_state_result state_switching_run(void *obj)
{
	struct main_state *s = obj;
	const struct network_msg *msg = network_msg_get(s);

	if (s->chan == &switch_timeout_chan) {
		const struct switch_timeout_msg *timeout =
			(const struct switch_timeout_msg *)s->msg_buf;

		if (timeout->id != s->switch_id) {
			return SMF_EVENT_HANDLED;
		}

		LOG_WRN("%s did not come up within %d s, staying on %s",
			network_iface_name(s->target), CONFIG_APP_MAIN_SWITCH_TIMEOUT_SECONDS,
			network_iface_name(s->active));
		s->mode = s->prev_mode;

		if (s->active != NETWORK_IFACE_NONE) {
			go_online(s, s->active);
		} else {
			smf_set_state(SMF_CTX(s), &states[STATE_OFFLINE]);
		}
		return SMF_EVENT_HANDLED;
	}

	if (msg == NULL) {
		return SMF_EVENT_PROPAGATE;
	}

	if (msg->type == NETWORK_IFACE_UP && msg->iface == s->target) {
		go_online(s, s->target);
		return SMF_EVENT_HANDLED;
	}

	if (msg->type == NETWORK_IFACE_DOWN && msg->iface == s->active) {
		LOG_INF("%s went down while switching", network_iface_name(s->active));
		s->active = NETWORK_IFACE_NONE;
		stream_request(STREAM_DISCONNECT, NETWORK_IFACE_NONE);
		return SMF_EVENT_HANDLED;
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_switching_exit(void *obj)
{
	ARG_UNUSED(obj);

	(void)k_work_cancel_delayable(&switch_timeout_work);
}

/* Kept outside the state machine so every state sees the same interface view. */
static void up_track(struct main_state *s)
{
	const struct network_msg *msg = network_msg_get(s);

	if (msg == NULL || msg->iface == NETWORK_IFACE_NONE) {
		return;
	}

	if (msg->type == NETWORK_IFACE_UP) {
		s->up |= BIT(msg->iface);
	} else if (msg->type == NETWORK_IFACE_DOWN) {
		s->up &= ~BIT(msg->iface);
	}
}

static void main_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Main watchdog expired, channel: %d, thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	FATAL_ERROR_WATCHDOG_TIMEOUT();
}

int main(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = CONFIG_APP_MAIN_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC;
	const uint32_t execution_time_ms =
		CONFIG_APP_MAIN_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC;
	const k_timeout_t zbus_wait = K_MSEC(wdt_timeout_ms - execution_time_ms);

	err = task_wdt_init(DEVICE_DT_GET(DT_ALIAS(watchdog0)));
	if (err) {
		LOG_ERR("task_wdt_init, error: %d", err);
		FATAL_ERROR();
		return -EFAULT;
	}

	task_wdt_id = task_wdt_add(wdt_timeout_ms, main_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("task_wdt_add, error: %d", task_wdt_id);
		FATAL_ERROR();
		return -EFAULT;
	}

	smf_set_initial(SMF_CTX(&main_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			FATAL_ERROR();
			return -EFAULT;
		}

		err = zbus_sub_wait_msg(&main_subscriber, &main_state.chan, main_state.msg_buf,
					zbus_wait);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			FATAL_ERROR();
			return -EFAULT;
		}

		up_track(&main_state);

		err = smf_run_state(SMF_CTX(&main_state));
		if (err) {
			LOG_ERR("smf_run_state, error: %d", err);
			FATAL_ERROR();
			return -EFAULT;
		}
	}

	return 0;
}
