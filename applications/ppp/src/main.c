/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>
#include <zephyr/sys/reboot.h>
#include <date_time.h>
#include <stdio.h>

#include "app_common.h"
#include "modules/network/network.h"
#include "modules/cloud/cloud.h"

#if defined(CONFIG_APP_FOTA)
#include "modules/fota/fota.h"
#endif /* CONFIG_APP_FOTA */

#if defined(CONFIG_APP_LOCATION)
#include "modules/location/location.h"
#endif /* CONFIG_APP_LOCATION */

#if defined(CONFIG_APP_BATTERY)
#include "modules/battery/battery.h"
#endif /* CONFIG_APP_BATTERY */

#if defined(CONFIG_APP_BUTTONS)
#include "modules/buttons/buttons.h"
#endif /* CONFIG_APP_BUTTONS */

#if defined(CONFIG_APP_LED)
#include "modules/led/led.h"
#endif /* CONFIG_APP_LED */

LOG_MODULE_REGISTER(main, CONFIG_APP_MAIN_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_MAIN_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_MAIN_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

ZBUS_MSG_SUBSCRIBER_DEFINE(main_subscriber);

/* Private channel message types for internal state management. */
enum priv_main_msg_type {
	MAIN_PRIV_CLOUD_SYNCHRONIZATION,
};

struct priv_main_msg {
	enum priv_main_msg_type type;
};

ZBUS_CHAN_DEFINE(main_priv_chan,
		 struct priv_main_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 */
#define CHANNEL_LIST(X) \
	X(network_chan, struct network_msg) \
	X(cloud_chan, struct cloud_msg) \
	IF_ENABLED(CONFIG_APP_FOTA, (X(fota_chan, struct fota_msg))) \
	IF_ENABLED(CONFIG_APP_LOCATION, (X(location_chan, struct location_msg))) \
	IF_ENABLED(CONFIG_APP_BUTTONS, (X(button_chan, struct button_msg))) \
	X(main_priv_chan, struct priv_main_msg)

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE		    MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add main_subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, main_subscriber, 0);

/* Expand to a call to ZBUS_CHAN_ADD_OBS for each channel in the list. */
CHANNEL_LIST(ADD_OBSERVERS)

/** Demo cloud payload template */
#define DEMO_CLOUD_PAYLOAD_FMT \
	"{\"appId\":\"SMHA\",\"messageType\":\"DATA\",\"data\":\"hello\",\"timestamp\":%lld}"

#if defined(CONFIG_APP_LED)
/** Slow red blink while the cloud is not connected. */
static const struct led_msg led_pattern_disconnected = {
	.type = LED_RGB_SET,
	.red = 255,
	.duration_on_msec = 250,
	.duration_off_msec = 5000,
	.repetitions = -1,
};

/** Fast green blink while a cloud synchronization is running. */
static const struct led_msg led_pattern_sync = {
	.type = LED_RGB_SET,
	.green = 255,
	.duration_on_msec = 250,
	.duration_off_msec = 250,
	.repetitions = -1,
};

/** Short yellow pulse every 15 seconds between synchronizations, to show the device is alive. */
static const struct led_msg led_pattern_idle = {
	.type = LED_RGB_SET,
	.red = 255,
	.green = 255,
	.duration_on_msec = 50,
	.duration_off_msec = 15000,
	.repetitions = -1,
};

/** Short purple pulse every second while a FOTA download is in progress. */
static const struct led_msg led_pattern_fota = {
	.type = LED_RGB_SET,
	.red = 255,
	.blue = 255,
	.duration_on_msec = 100,
	.duration_off_msec = 900,
	.repetitions = -1,
};
#endif /* CONFIG_APP_LED */

/** Application SMF states. */
enum main_app_state {
	/**
	 * Top-level state.
	 * The initial substate is @ref STATE_CLOUD_DISCONNECTED.
	 */
	STATE_RUNNING,
		/**
		 * Cloud is not connected. Periodic cloud synchronization is not
		 * scheduled, and synchronization triggers are ignored.
		 */
		STATE_CLOUD_DISCONNECTED,
		/**
		 * Cloud is connected. Periodic synchronization is scheduled on the
		 * dedicated cloud-sync workqueue.
		 *
		 * The initial substate is @ref STATE_SYNC_IDLE. Each synchronization
		 * advances through the sync substates in order, waiting for each step
		 * to complete before starting the next.
		 */
		STATE_CLOUD_CONNECTED,
			/** Waiting for the next synchronization trigger. */
			STATE_SYNC_IDLE,
			/**
			 * A synchronization is running. The initial substate is
			 * @ref STATE_SYNC_DEMO.
			 */
			STATE_SYNCING,
				/** Sending the demo cloud payload. */
				STATE_SYNC_DEMO,
#if defined(CONFIG_APP_LOCATION)
				/** Collecting location measurements and resolving location. */
				STATE_SYNC_LOCATION,
#endif /* CONFIG_APP_LOCATION */
				/** Polling the device shadow. */
				STATE_SYNC_SHADOW,
#if defined(CONFIG_APP_FOTA)
				/** Polling for a FOTA job. */
				STATE_SYNC_FOTA,
#endif /* CONFIG_APP_FOTA */
				/** Posting pending Memfault data. */
				STATE_SYNC_MEMFAULT,
#if defined(CONFIG_APP_FOTA)
		/**
		 * A FOTA download is in progress. Cloud state at entry is saved in
		 * @ref main_state::cloud_history for restore on @ref FOTA_ABORTED.
		 */
		STATE_FOTA,
	/**
	 * FOTA completed; application cleanup runs before reboot.
	 */
	STATE_REBOOTING,
#endif /* CONFIG_APP_FOTA */
};

/* State object for the main module.
 * Used to transfer data between state changes.
 */
struct main_state {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	struct k_work_delayable cloud_sync_dwork;
	/** Cloud state to restore after @ref FOTA_ABORTED. */
	enum main_app_state cloud_history;
};

static struct main_state main_state;
static const struct smf_state states[];

/** Cloud synchronization workqueue */
static K_THREAD_STACK_DEFINE(cloud_sync_workq_stack,
			     CONFIG_APP_MAIN_CLOUD_SYNC_WORKQ_STACK_SIZE);
static struct k_work_q cloud_sync_workq;

#if defined(CONFIG_APP_BATTERY)
static void battery_sample_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(battery_sample_work, battery_sample_handler);
#endif /* CONFIG_APP_BATTERY */

/** Forward declarations */

static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_cloud_disconnected_entry(void *obj);
static enum smf_state_result state_cloud_disconnected_run(void *obj);
static void state_cloud_disconnected_exit(void *obj);
static void state_cloud_connected_entry(void *obj);
static enum smf_state_result state_cloud_connected_run(void *obj);
static void state_cloud_connected_exit(void *obj);
static void state_sync_idle_entry(void *obj);
static enum smf_state_result state_sync_idle_run(void *obj);
static void state_syncing_entry(void *obj);
static void state_sync_demo_entry(void *obj);
static enum smf_state_result state_sync_demo_run(void *obj);

#if defined(CONFIG_APP_LOCATION)
static void state_sync_location_entry(void *obj);
static enum smf_state_result state_sync_location_run(void *obj);

#endif /* CONFIG_APP_LOCATION */
static void state_sync_shadow_entry(void *obj);
static enum smf_state_result state_sync_shadow_run(void *obj);

#if defined(CONFIG_APP_FOTA)
static void state_sync_fota_entry(void *obj);
static enum smf_state_result state_sync_fota_run(void *obj);
#endif /* CONFIG_APP_FOTA */

static void state_sync_memfault_entry(void *obj);
static enum smf_state_result state_sync_memfault_run(void *obj);

#if defined(CONFIG_APP_FOTA)
static void state_fota_entry(void *obj);
static enum smf_state_result state_fota_run(void *obj);
static void state_rebooting_entry(void *obj);
#endif /* CONFIG_APP_FOTA */

/** SMF state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry,
				 state_running_run,
				 NULL,
				 NULL,
				 &states[STATE_CLOUD_DISCONNECTED]),
	[STATE_CLOUD_DISCONNECTED] =
		SMF_CREATE_STATE(state_cloud_disconnected_entry,
				 state_cloud_disconnected_run,
				 state_cloud_disconnected_exit,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_CLOUD_CONNECTED] =
		SMF_CREATE_STATE(state_cloud_connected_entry,
				 state_cloud_connected_run,
				 state_cloud_connected_exit,
				 &states[STATE_RUNNING],
				 &states[STATE_SYNC_IDLE]),
	[STATE_SYNC_IDLE] =
		SMF_CREATE_STATE(state_sync_idle_entry,
				 state_sync_idle_run,
				 NULL,
				 &states[STATE_CLOUD_CONNECTED],
				 NULL),
	[STATE_SYNCING] =
		SMF_CREATE_STATE(state_syncing_entry,
				 NULL,
				 NULL,
				 &states[STATE_CLOUD_CONNECTED],
				 &states[STATE_SYNC_DEMO]),
	[STATE_SYNC_DEMO] =
		SMF_CREATE_STATE(state_sync_demo_entry,
				 state_sync_demo_run,
				 NULL,
				 &states[STATE_SYNCING],
				 NULL),
#if defined(CONFIG_APP_LOCATION)
	[STATE_SYNC_LOCATION] =
		SMF_CREATE_STATE(state_sync_location_entry,
				 state_sync_location_run,
				 NULL,
				 &states[STATE_SYNCING],
				 NULL),
#endif /* CONFIG_APP_LOCATION */
	[STATE_SYNC_SHADOW] =
		SMF_CREATE_STATE(state_sync_shadow_entry,
				 state_sync_shadow_run,
				 NULL,
				 &states[STATE_SYNCING],
				 NULL),
#if defined(CONFIG_APP_FOTA)
	[STATE_SYNC_FOTA] =
		SMF_CREATE_STATE(state_sync_fota_entry,
				 state_sync_fota_run,
				 NULL,
				 &states[STATE_SYNCING],
				 NULL),
#endif /* CONFIG_APP_FOTA */
	[STATE_SYNC_MEMFAULT] =
		SMF_CREATE_STATE(state_sync_memfault_entry,
				 state_sync_memfault_run,
				 NULL,
				 &states[STATE_SYNCING],
				 NULL),
#if defined(CONFIG_APP_FOTA)
	[STATE_FOTA] =
		SMF_CREATE_STATE(state_fota_entry,
				 state_fota_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_REBOOTING] =
		SMF_CREATE_STATE(state_rebooting_entry,
				 NULL, NULL, NULL, NULL),
#endif /* CONFIG_APP_FOTA */
};

/** Convenience functions */

static void demo_cloud_message_send(void)
{
	int err;
	int64_t unix_time_ms;
	struct cloud_msg msg = {
		.type = CLOUD_MESSAGE_SEND,
	};

	err = date_time_now(&unix_time_ms);
	if (err) {
		LOG_ERR("date_time_now, error: %d", err);
		FATAL_ERROR();
	}

	err = snprintk(msg.payload, sizeof(msg.payload), DEMO_CLOUD_PAYLOAD_FMT,
		       (long long)unix_time_ms);
	if (err < 0 || err >= (int)sizeof(msg.payload)) {
		LOG_ERR("Demo payload formatting failed, error: %d", err);
		FATAL_ERROR();
	}

	msg.payload_len = (size_t)err;
	LOG_INF("Sending demo cloud payload: %s", msg.payload);

	err = zbus_chan_pub(&cloud_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		FATAL_ERROR();
	}
}

static void cloud_connect_request(void)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_CONNECT
	};

	err = zbus_chan_pub(&cloud_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		FATAL_ERROR();
	}
}

static void cloud_disconnect_request(void)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_DISCONNECT
	};

	err = zbus_chan_pub(&cloud_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		FATAL_ERROR();
	}
}

#if defined(CONFIG_APP_LOCATION)
static void location_search_request(void)
{
	int err;
	struct location_msg msg = {
		.type = LOCATION_SEARCH_TRIGGER
	};

	err = zbus_chan_pub(&location_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		FATAL_ERROR();
	}
}

static void location_request_forward(const struct location_search_wifi_cell_result *request)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_LOCATION_REQUEST
	};

	msg.location_request = *request;

	err = zbus_chan_pub(&cloud_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		FATAL_ERROR();
	}
}
#endif /* CONFIG_APP_LOCATION */

#if defined(CONFIG_APP_FOTA)
static void fota_poll_request(void)
{
	int err;
	struct fota_msg fota_msg = {
		.type = FOTA_POLL_REQUEST
	};

	err = zbus_chan_pub(&fota_chan, &fota_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		FATAL_ERROR();
	}
}
#endif /* CONFIG_APP_FOTA */

static void shadow_poll_request(void)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_SHADOW_POLL_REQUEST
	};

	err = zbus_chan_pub(&cloud_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		FATAL_ERROR();
	}
}

static void memfault_post_request(void)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_MEMFAULT_POST_REQUEST
	};

	err = zbus_chan_pub(&cloud_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		FATAL_ERROR();
	}
}

static void cloud_sync_schedule(struct main_state *state)
{
	int err;

	err = k_work_schedule_for_queue(&cloud_sync_workq, &state->cloud_sync_dwork,
					K_SECONDS(CONFIG_APP_MAIN_SYNC_INTERVAL_SECONDS));
	if (err < 0) {
		LOG_ERR("k_work_schedule_for_queue, error: %d", err);
		FATAL_ERROR();
	}
}

static void cloud_sync_cancel(struct main_state *state)
{
	(void)k_work_cancel_delayable(&state->cloud_sync_dwork);
}

static void cloud_sync_delayed_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct main_state *state = CONTAINER_OF(dwork, struct main_state, cloud_sync_dwork);
	int err;
	struct priv_main_msg msg = {
		.type = MAIN_PRIV_CLOUD_SYNCHRONIZATION
	};

	err = zbus_chan_pub(&main_priv_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub main_priv_chan, error: %d", err);
		FATAL_ERROR();
	}

	cloud_sync_schedule(state);
}

#if defined(CONFIG_APP_BUTTONS)
static void cloud_sync_trigger(struct main_state *state)
{
	int err;

	err = k_work_reschedule_for_queue(&cloud_sync_workq, &state->cloud_sync_dwork,
					  K_NO_WAIT);
	if (err < 0) {
		LOG_ERR("k_work_reschedule_for_queue, error: %d", err);
		FATAL_ERROR();
	}
}
#endif /* CONFIG_APP_BUTTONS */

#if defined(CONFIG_APP_BATTERY)
static void battery_sample_handler(struct k_work *work)
{
	struct battery_msg msg = { .type = BATTERY_SAMPLE };
	int err;

	err = zbus_chan_pub(&battery_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub battery_chan, error: %d", err);
		FATAL_ERROR();
	}

	err = k_work_reschedule(&battery_sample_work,
				K_SECONDS(CONFIG_APP_BATTERY_SAMPLE_INTERVAL_SECONDS));
	if (err < 0) {
		LOG_ERR("k_work_reschedule battery_sample_work, error: %d", err);
		FATAL_ERROR();
	}
}
#endif /* CONFIG_APP_BATTERY */

#if defined(CONFIG_APP_LED)
static void led_pattern_set(const struct led_msg *pattern)
{
	int err;

	err = zbus_chan_pub(&led_chan, pattern, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub led_chan, error: %d", err);
		FATAL_ERROR();
	}
}
#endif /* CONFIG_APP_LED */

#if defined(CONFIG_APP_FOTA)
static void fota_running_state_restore(struct main_state *state_object)
{
	if (state_object->cloud_history == STATE_CLOUD_CONNECTED) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CLOUD_CONNECTED]);
	} else {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CLOUD_DISCONNECTED]);
	}
}
#endif /* CONFIG_APP_FOTA */

/** SMF state functions */

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_running_entry");
}

static enum smf_state_result state_running_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg =
			(const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			LOG_INF("Network connected");
			cloud_connect_request();
		} else if (msg->type == NETWORK_DISCONNECTED) {
			LOG_INF("Network disconnected");
			cloud_disconnect_request();
		}

		return SMF_EVENT_HANDLED;
	}

#if defined(CONFIG_APP_LOCATION)
	if (state_object->chan == &location_chan) {
		const struct location_msg *msg =
			(const struct location_msg *)state_object->msg_buf;

		if (msg->type == LOCATION_SEARCH_WIFI_CELL_RESULT) {
			location_request_forward(&msg->wifi_cell_result);
		}

		return SMF_EVENT_HANDLED;
	}
#endif /* CONFIG_APP_LOCATION */

#if defined(CONFIG_APP_FOTA)
	if (state_object->chan == &fota_chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		if (msg->type == FOTA_STARTING) {
			LOG_INF("FOTA download starting");
			smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA]);

			return SMF_EVENT_HANDLED;
		}
	}
#endif /* CONFIG_APP_FOTA */

#if defined(CONFIG_APP_BUTTONS)
	if (state_object->chan == &button_chan) {
		const struct button_msg *msg = (const struct button_msg *)state_object->msg_buf;

		if (msg->type == BUTTON_1 || msg->type == BUTTON_2) {
			LOG_WRN("Button %u: cloud not available, ignoring", msg->type);
		} else {
			LOG_WRN("Button %u: no action defined", msg->type);
		}

		return SMF_EVENT_HANDLED;
	}
#endif /* CONFIG_APP_BUTTONS */

	return SMF_EVENT_PROPAGATE;
}

static void state_cloud_disconnected_entry(void *obj)
{
	struct main_state *state_object = obj;

	LOG_INF("state_cloud_disconnected_entry");

	state_object->cloud_history = STATE_CLOUD_DISCONNECTED;

#if defined(CONFIG_APP_LED)
	led_pattern_set(&led_pattern_disconnected);
#endif /* CONFIG_APP_LED */
}

static void state_cloud_disconnected_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_cloud_disconnected_exit");
}

static enum smf_state_result state_cloud_disconnected_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)state_object->msg_buf;

		if (msg->type == CLOUD_CONNECTED) {
			LOG_INF("Cloud connected");

			smf_set_state(SMF_CTX(state_object), &states[STATE_CLOUD_CONNECTED]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_cloud_connected_entry(void *obj)
{
	struct main_state *state_object = obj;

	LOG_INF("state_cloud_connected_entry");

	state_object->cloud_history = STATE_CLOUD_CONNECTED;
	cloud_sync_schedule(state_object);

	/* Synchronize once immediately so connecting always syncs without waiting a period. */
	smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_DEMO]);
}

static void state_cloud_connected_exit(void *obj)
{
	struct main_state *state_object = obj;

	LOG_INF("state_cloud_connected_exit");

	cloud_sync_cancel(state_object);
}

static enum smf_state_result state_cloud_connected_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)state_object->msg_buf;

		if (msg->type == CLOUD_DISCONNECTED) {
			LOG_INF("Cloud disconnected");

			smf_set_state(SMF_CTX(state_object), &states[STATE_CLOUD_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_APP_BUTTONS)
	if (state_object->chan == &button_chan) {
		const struct button_msg *msg = (const struct button_msg *)state_object->msg_buf;

		if (msg->type == BUTTON_1 || msg->type == BUTTON_2) {
			LOG_WRN("Button %u: cloud synchronization in progress, ignoring",
				msg->type);

			return SMF_EVENT_HANDLED;
		}
	}
#endif /* CONFIG_APP_BUTTONS */

	return SMF_EVENT_PROPAGATE;
}

static void state_sync_idle_entry(void *obj)
{
	struct main_state *state_object = obj;

	cloud_sync_schedule(state_object);

#if defined(CONFIG_APP_LED)
	led_pattern_set(&led_pattern_idle);
#endif /* CONFIG_APP_LED */
}

static enum smf_state_result state_sync_idle_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &main_priv_chan) {
		const struct priv_main_msg *msg =
			(const struct priv_main_msg *)state_object->msg_buf;

		if (msg->type == MAIN_PRIV_CLOUD_SYNCHRONIZATION) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_DEMO]);

			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_APP_BUTTONS)
	if (state_object->chan == &button_chan) {
		const struct button_msg *msg = (const struct button_msg *)state_object->msg_buf;

		switch (msg->type) {
		case BUTTON_1:
			LOG_INF("Button 1: cloud synchronization requested");
			cloud_sync_trigger(state_object);

			return SMF_EVENT_HANDLED;
		case BUTTON_2:
			LOG_INF("Button 2: Memfault heartbeat and data post requested");
			memfault_post_request();

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}
#endif /* CONFIG_APP_BUTTONS */

	return SMF_EVENT_PROPAGATE;
}

static void state_syncing_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_syncing_entry");

#if defined(CONFIG_APP_LED)
	led_pattern_set(&led_pattern_sync);
#endif /* CONFIG_APP_LED */
}

static void state_sync_demo_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_sync_demo_entry");

	demo_cloud_message_send();
}

static enum smf_state_result state_sync_demo_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)state_object->msg_buf;

		if (msg->type == CLOUD_MESSAGE_SENT) {
#if defined(CONFIG_APP_LOCATION)
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_LOCATION]);
#else
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_SHADOW]);
#endif /* CONFIG_APP_LOCATION */

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

#if defined(CONFIG_APP_LOCATION)
static void state_sync_location_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_sync_location_entry");

	location_search_request();
}

static enum smf_state_result state_sync_location_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &location_chan) {
		const struct location_msg *msg =
			(const struct location_msg *)state_object->msg_buf;

		if (msg->type == LOCATION_SEARCH_DONE) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_SHADOW]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}
#endif /* CONFIG_APP_LOCATION */

static void state_sync_shadow_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_sync_shadow_entry");

	shadow_poll_request();
}

static enum smf_state_result state_sync_shadow_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)state_object->msg_buf;

		if (msg->type == CLOUD_SHADOW_POLLED) {
#if defined(CONFIG_APP_FOTA)
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_FOTA]);
#else
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_MEMFAULT]);
#endif /* CONFIG_APP_FOTA */

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

#if defined(CONFIG_APP_FOTA)
static void state_sync_fota_entry(void *obj)
{
	struct main_state *state_object = obj;

	LOG_INF("state_sync_fota_entry");

	/* FOTA poll and download can run for minutes; stop the sync timer so
	 * demo/shadow/memfault CoAP traffic does not contend with the download.
	 */
	cloud_sync_cancel(state_object);
	fota_poll_request();
}

static enum smf_state_result state_sync_fota_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &fota_chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		/* FOTA_STARTING propagates to STATE_RUNNING, which enters STATE_FOTA. */
		if (msg->type == FOTA_ABORTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_MEMFAULT]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}
#endif /* CONFIG_APP_FOTA */

static void state_sync_memfault_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_sync_memfault_entry");

	memfault_post_request();
}

static enum smf_state_result state_sync_memfault_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)state_object->msg_buf;

		if (msg->type == CLOUD_MEMFAULT_POSTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_IDLE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

#if defined(CONFIG_APP_FOTA)
static void state_fota_entry(void *obj)
{
	struct main_state *state_object = obj;

	LOG_INF("state_fota_entry");
	cloud_sync_cancel(state_object);

#if defined(CONFIG_APP_LED)
	led_pattern_set(&led_pattern_fota);
#endif /* CONFIG_APP_LED */
}

static enum smf_state_result state_fota_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &fota_chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		if (msg->type == FOTA_ABORTED) {
			LOG_INF("FOTA download aborted");
			fota_running_state_restore(state_object);

			return SMF_EVENT_HANDLED;
		} else if (msg->type == FOTA_REBOOT_REQUEST) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_REBOOTING]);

			return SMF_EVENT_HANDLED;
		}
	} else if (state_object->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)state_object->msg_buf;

		if (msg->type == CLOUD_DISCONNECTED) {
			state_object->cloud_history = STATE_CLOUD_DISCONNECTED;

			return SMF_EVENT_HANDLED;
		} else if (msg->type == CLOUD_CONNECTED) {
			state_object->cloud_history = STATE_CLOUD_CONNECTED;

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_rebooting_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_rebooting_entry");
	LOG_INF("FOTA successful, rebooting to apply the update");
	LOG_PANIC();

	sys_reboot(SYS_REBOOT_COLD);
}
#endif /* CONFIG_APP_FOTA */

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
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_MAIN_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_MAIN_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct k_work_queue_config cloud_sync_workq_cfg = {
		.name = "cloud_sync",
	};

	k_work_queue_init(&cloud_sync_workq);
	k_work_queue_start(&cloud_sync_workq,
			   cloud_sync_workq_stack,
			   K_THREAD_STACK_SIZEOF(cloud_sync_workq_stack),
			   CONFIG_APP_MAIN_CLOUD_SYNC_WORKQ_PRIORITY,
			   &cloud_sync_workq_cfg);
	k_work_init_delayable(&main_state.cloud_sync_dwork, cloud_sync_delayed_work_handler);

	err = task_wdt_init(DEVICE_DT_GET(DT_ALIAS(watchdog0)));
	if (err) {
		LOG_ERR("task_wdt_init, error: %d", err);
		FATAL_ERROR();
		return -EFAULT;
	}

	task_wdt_id = task_wdt_add(wdt_timeout_ms, main_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		FATAL_ERROR();
		return -EFAULT;
	}

#if defined(CONFIG_APP_BATTERY)
	err = k_work_reschedule(&battery_sample_work, K_NO_WAIT);
	if (err < 0) {
		LOG_ERR("k_work_reschedule battery_sample_work, error: %d", err);
		FATAL_ERROR();
		return -EFAULT;
	}
#endif /* CONFIG_APP_BATTERY */

	smf_set_initial(SMF_CTX(&main_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			FATAL_ERROR();
			return -EFAULT;
		}

		err = zbus_sub_wait_msg(&main_subscriber, &main_state.chan,
					main_state.msg_buf, zbus_wait);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			FATAL_ERROR();
			return -EFAULT;
		}

		err = smf_run_state(SMF_CTX(&main_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			FATAL_ERROR();
			return -EFAULT;
		}
	}

	return 0;
}
