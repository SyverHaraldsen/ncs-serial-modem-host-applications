/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>

#include "app_common.h"
#include "modules/network/network.h"

#if defined(CONFIG_APP_LOCATION)
#include "modules/location/location.h"
#endif /* CONFIG_APP_LOCATION */

#if defined(CONFIG_APP_BATTERY)
#include "modules/battery/battery.h"
#include "modules/cloud/cloud.h"
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
	     "Watchdog timeout must exceed the maximum message processing time");

#if defined(CONFIG_APP_LOCATION) || defined(CONFIG_APP_BATTERY)
#define HAS_SYNC_STEPS 1
#endif

ZBUS_MSG_SUBSCRIBER_DEFINE(main_subscriber);

/* Private channel message types for internal state management. */
enum priv_main_msg_type {
	MAIN_PRIV_SYNC,
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

#define CHANNEL_LIST(X) \
	X(main_priv_chan, struct priv_main_msg) \
	X(network_chan, struct network_msg) \
	IF_ENABLED(CONFIG_APP_LOCATION, (X(location_chan, struct location_msg))) \
	IF_ENABLED(CONFIG_APP_BATTERY, (X(cloud_chan, struct cloud_msg))) \
	IF_ENABLED(CONFIG_APP_BUTTONS, (X(button_chan, struct button_msg)))

#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, main_subscriber, 0);
CHANNEL_LIST(ADD_OBSERVERS)

#if defined(CONFIG_APP_LED)
/** Slow red blink while the network is down. */
static const struct led_msg led_pattern_disconnected = {
	.type = LED_RGB_SET,
	.red = 255,
	.duration_on_msec = 250,
	.duration_off_msec = 5000,
	.repetitions = -1,
};

/** Fast green blink while a synchronization is running. */
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
#endif /* CONFIG_APP_LED */

/** Application SMF states. */
enum main_app_state {
	/**
	 * Top-level state. Consumes the messages that no substate acts on.
	 * The initial substate is @ref STATE_NETWORK_DISCONNECTED.
	 */
	STATE_RUNNING,
		/**
		 * Network is down. Synchronization is not scheduled, and
		 * synchronization triggers are ignored.
		 */
		STATE_NETWORK_DISCONNECTED,
		/**
		 * Network is up. The first synchronization runs after
		 * CONFIG_APP_SYNC_BOOT_DELAY_SECONDS, then every CONFIG_APP_SYNC_INTERVAL.
		 *
		 * The initial substate is @ref STATE_SYNC_IDLE. Each synchronization
		 * advances through the sync substates in order, waiting for each step
		 * to complete before starting the next.
		 */
		STATE_NETWORK_CONNECTED,
			/** Waiting for the next synchronization trigger. */
			STATE_SYNC_IDLE,
#if defined(HAS_SYNC_STEPS)
			/**
			 * A synchronization is running. Triggers are ignored until it
			 * returns to @ref STATE_SYNC_IDLE. The initial substate is the
			 * first enabled sync step.
			 */
			STATE_SYNCING,
#endif /* HAS_SYNC_STEPS */
#if defined(CONFIG_APP_LOCATION)
				/** Requesting a location fix from the modem nRF Cloud client. */
				STATE_SYNC_LOCATION,
#endif /* CONFIG_APP_LOCATION */
#if defined(CONFIG_APP_BATTERY)
				/** Reporting the battery state of charge to nRF Cloud. */
				STATE_SYNC_BATTERY,
#endif /* CONFIG_APP_BATTERY */
};

#if defined(CONFIG_APP_LOCATION)
#define STATE_SYNC_FIRST_STEP STATE_SYNC_LOCATION
#elif defined(CONFIG_APP_BATTERY)
#define STATE_SYNC_FIRST_STEP STATE_SYNC_BATTERY
#endif

/* State object for the main module. */
struct main_state {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
};

static struct main_state main_state;
static const struct smf_state states[];

static void sync_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sync_work, sync_work_handler);

/** Forward declarations */

static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_network_disconnected_entry(void *obj);
static enum smf_state_result state_network_disconnected_run(void *obj);
static void state_network_connected_entry(void *obj);
static enum smf_state_result state_network_connected_run(void *obj);
static void state_network_connected_exit(void *obj);
static void state_sync_idle_entry(void *obj);
static enum smf_state_result state_sync_idle_run(void *obj);

#if defined(HAS_SYNC_STEPS)
static void state_syncing_entry(void *obj);
static enum smf_state_result state_syncing_run(void *obj);
#endif /* HAS_SYNC_STEPS */

#if defined(CONFIG_APP_LOCATION)
static void state_sync_location_entry(void *obj);
static enum smf_state_result state_sync_location_run(void *obj);
#endif /* CONFIG_APP_LOCATION */

#if defined(CONFIG_APP_BATTERY)
static void state_sync_battery_entry(void *obj);
static enum smf_state_result state_sync_battery_run(void *obj);
#endif /* CONFIG_APP_BATTERY */

/** SMF state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry,
				 state_running_run,
				 NULL,
				 NULL,
				 &states[STATE_NETWORK_DISCONNECTED]),
	[STATE_NETWORK_DISCONNECTED] =
		SMF_CREATE_STATE(state_network_disconnected_entry,
				 state_network_disconnected_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_NETWORK_CONNECTED] =
		SMF_CREATE_STATE(state_network_connected_entry,
				 state_network_connected_run,
				 state_network_connected_exit,
				 &states[STATE_RUNNING],
				 &states[STATE_SYNC_IDLE]),
	[STATE_SYNC_IDLE] =
		SMF_CREATE_STATE(state_sync_idle_entry,
				 state_sync_idle_run,
				 NULL,
				 &states[STATE_NETWORK_CONNECTED],
				 NULL),
#if defined(HAS_SYNC_STEPS)
	[STATE_SYNCING] =
		SMF_CREATE_STATE(state_syncing_entry,
				 state_syncing_run,
				 NULL,
				 &states[STATE_NETWORK_CONNECTED],
				 &states[STATE_SYNC_FIRST_STEP]),
#endif /* HAS_SYNC_STEPS */
#if defined(CONFIG_APP_LOCATION)
	[STATE_SYNC_LOCATION] =
		SMF_CREATE_STATE(state_sync_location_entry,
				 state_sync_location_run,
				 NULL,
				 &states[STATE_SYNCING],
				 NULL),
#endif /* CONFIG_APP_LOCATION */
#if defined(CONFIG_APP_BATTERY)
	[STATE_SYNC_BATTERY] =
		SMF_CREATE_STATE(state_sync_battery_entry,
				 state_sync_battery_run,
				 NULL,
				 &states[STATE_SYNCING],
				 NULL),
#endif /* CONFIG_APP_BATTERY */
};

/** Convenience functions */

static void sync_schedule(k_timeout_t delay)
{
	int err;

	err = k_work_reschedule(&sync_work, delay);
	if (err < 0) {
		LOG_ERR("k_work_reschedule sync_work, error: %d", err);
		FATAL_ERROR();
	}
}

static void sync_cancel(void)
{
	(void)k_work_cancel_delayable(&sync_work);
}

/* Publishes a sync trigger and schedules the next one, so the interval is kept even when a
 * trigger arrives while a synchronization is running and is ignored.
 */
static void sync_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct priv_main_msg msg = { .type = MAIN_PRIV_SYNC };
	int err;

	err = zbus_chan_pub(&main_priv_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub main_priv_chan, error: %d", err);
		FATAL_ERROR();
	}

	sync_schedule(K_SECONDS(CONFIG_APP_SYNC_INTERVAL));
}

#if defined(CONFIG_APP_LOCATION)
static void location_fix_request(void)
{
	struct location_msg msg = { .type = LOCATION_FIX_REQUEST };
	int err;

	err = zbus_chan_pub(&location_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub location_chan, error: %d", err);
		FATAL_ERROR();
	}
}
#endif /* CONFIG_APP_LOCATION */

#if defined(CONFIG_APP_BATTERY)
static void battery_report_request(int percent)
{
	struct cloud_msg msg = {
		.type = CLOUD_BATTERY_SAMPLE,
		.battery_percent = percent,
	};
	int err;

	err = zbus_chan_pub(&cloud_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub cloud_chan, error: %d", err);
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

#if defined(CONFIG_APP_BUTTONS)
/* Returns true if the message on button_chan is a Button 1 press, the sync trigger. */
static bool is_sync_button(const struct main_state *state_object)
{
	const struct button_msg *msg = (const struct button_msg *)state_object->msg_buf;

	return state_object->chan == &button_chan && msg->type == BUTTON_1;
}
#endif /* CONFIG_APP_BUTTONS */

/** SMF state functions */

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_running_entry");
}

static enum smf_state_result state_running_run(void *obj)
{
#if defined(CONFIG_APP_BUTTONS)
	struct main_state *state_object = obj;

	if (state_object->chan == &button_chan) {
		const struct button_msg *msg = (const struct button_msg *)state_object->msg_buf;

		LOG_WRN("Button %u: no action defined", msg->type);

		return SMF_EVENT_HANDLED;
	}
#else
	ARG_UNUSED(obj);
#endif /* CONFIG_APP_BUTTONS */

	/* Requests echoed back on module channels, completions that arrive after the network
	 * went down, and link state reports that do not change the state are consumed here.
	 */
	return SMF_EVENT_HANDLED;
}

static void state_network_disconnected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_network_disconnected_entry");
	LOG_INF("Network disconnected");

#if defined(CONFIG_APP_LED)
	led_pattern_set(&led_pattern_disconnected);
#endif /* CONFIG_APP_LED */
}

static enum smf_state_result state_network_disconnected_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_NETWORK_CONNECTED]);

			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_APP_BUTTONS)
	if (is_sync_button(state_object)) {
		LOG_INF("Button 1: network is down, sync skipped");

		return SMF_EVENT_HANDLED;
	}
#endif /* CONFIG_APP_BUTTONS */

	return SMF_EVENT_PROPAGATE;
}

static void state_network_connected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_network_connected_entry");
	LOG_INF("Network connected, first sync in %d s", CONFIG_APP_SYNC_BOOT_DELAY_SECONDS);

	sync_schedule(K_SECONDS(CONFIG_APP_SYNC_BOOT_DELAY_SECONDS));
}

static enum smf_state_result state_network_connected_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_NETWORK_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_network_connected_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("state_network_connected_exit");

	sync_cancel();
}

static void state_sync_idle_entry(void *obj)
{
	ARG_UNUSED(obj);

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

		if (msg->type == MAIN_PRIV_SYNC) {
			LOG_DBG("Sync tick, next in %d s", CONFIG_APP_SYNC_INTERVAL);
#if defined(HAS_SYNC_STEPS)
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNCING]);
#endif /* HAS_SYNC_STEPS */

			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_APP_BUTTONS)
	if (is_sync_button(state_object)) {
		LOG_INF("Button 1: sync requested");
		sync_schedule(K_NO_WAIT);

		return SMF_EVENT_HANDLED;
	}
#endif /* CONFIG_APP_BUTTONS */

	return SMF_EVENT_PROPAGATE;
}

#if defined(HAS_SYNC_STEPS)
static void state_syncing_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("state_syncing_entry");

#if defined(CONFIG_APP_LED)
	led_pattern_set(&led_pattern_sync);
#endif /* CONFIG_APP_LED */
}

static enum smf_state_result state_syncing_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &main_priv_chan) {
		LOG_WRN("Sync requested while a sync is running, ignoring");

		return SMF_EVENT_HANDLED;
	}

#if defined(CONFIG_APP_BUTTONS)
	if (is_sync_button(state_object)) {
		LOG_INF("Button 1: sync is running, sync skipped");

		return SMF_EVENT_HANDLED;
	}
#endif /* CONFIG_APP_BUTTONS */

	return SMF_EVENT_PROPAGATE;
}
#endif /* HAS_SYNC_STEPS */

#if defined(CONFIG_APP_LOCATION)
static void state_sync_location_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("state_sync_location_entry");

	location_fix_request();
}

static enum smf_state_result state_sync_location_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &location_chan) {
		const struct location_msg *msg =
			(const struct location_msg *)state_object->msg_buf;

		if (msg->type == LOCATION_FIX_DONE) {
#if defined(CONFIG_APP_BATTERY)
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_BATTERY]);
#else
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_IDLE]);
#endif /* CONFIG_APP_BATTERY */

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}
#endif /* CONFIG_APP_LOCATION */

#if defined(CONFIG_APP_BATTERY)
static void state_sync_battery_entry(void *obj)
{
	struct main_state *state_object = obj;
	int percent;
	int err;

	LOG_DBG("state_sync_battery_entry");

	err = battery_percent_get(&percent);
	if (err) {
		LOG_WRN("battery_percent_get, error: %d", err);
		smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_IDLE]);

		return;
	}

	battery_report_request(percent);
}

static enum smf_state_result state_sync_battery_run(void *obj)
{
	struct main_state *state_object = obj;

	if (state_object->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)state_object->msg_buf;

		if (msg->type == CLOUD_BATTERY_DONE) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_SYNC_IDLE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}
#endif /* CONFIG_APP_BATTERY */

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
			LOG_ERR("smf_run_state, error: %d", err);
			FATAL_ERROR();
			return -EFAULT;
		}
	}

	return 0;
}
