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
#endif
#if defined(CONFIG_APP_BATTERY)
#include "modules/battery/battery.h"
#include "modules/cloud/cloud.h"
#endif
#if defined(CONFIG_APP_BUTTONS)
#include "modules/buttons/buttons.h"
#endif
#if defined(CONFIG_APP_LED)
#include "modules/led/led.h"
#endif

LOG_MODULE_REGISTER(main, CONFIG_APP_MAIN_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_MAIN_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_MAIN_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must exceed the maximum message processing time");

enum main_msg_type {
	MAIN_SYNC,
};

struct main_msg {
	enum main_msg_type type;
};

ZBUS_CHAN_DEFINE(main_chan, struct main_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));
ZBUS_MSG_SUBSCRIBER_DEFINE(main_subscriber);

#define CHANNEL_LIST(X) \
	X(main_chan, struct main_msg) \
	X(network_chan, struct network_msg) \
	IF_ENABLED(CONFIG_APP_LOCATION, (X(location_chan, struct location_msg))) \
	IF_ENABLED(CONFIG_APP_BATTERY, (X(cloud_chan, struct cloud_msg))) \
	IF_ENABLED(CONFIG_APP_BUTTONS, (X(button_chan, struct button_msg)))

#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

#define ADD_OBSERVER(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, main_subscriber, 0);
CHANNEL_LIST(ADD_OBSERVER)

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
#endif

enum app_state {
	STATE_DISCONNECTED,
	STATE_CONNECTED,
		/** Waiting for the next synchronization trigger. */
		STATE_SYNC_IDLE,
		/** Waiting for the modules to finish the requested synchronization steps. */
		STATE_SYNCING,
};

struct app_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	/** Number of synchronization steps that have not reported completion yet. */
	uint8_t sync_pending;
};

static void telemetry_timer_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(telemetry_timer, telemetry_timer_fn);

#if defined(CONFIG_APP_BATTERY)
static void sample_timer_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sample_timer, sample_timer_fn);
#endif

static uint8_t report_telemetry(void);
static void disconnected_entry(void *obj);
static enum smf_state_result disconnected_run(void *obj);
static void connected_entry(void *obj);
static enum smf_state_result connected_run(void *obj);
static void connected_exit(void *obj);
static void sync_idle_entry(void *obj);
static enum smf_state_result sync_idle_run(void *obj);
static void syncing_entry(void *obj);
static enum smf_state_result syncing_run(void *obj);
static void wdt_callback(int channel_id, void *user_data);

static const struct smf_state states[] = {
	[STATE_DISCONNECTED] = SMF_CREATE_STATE(disconnected_entry, disconnected_run,
						NULL, NULL, NULL),
	[STATE_CONNECTED] = SMF_CREATE_STATE(connected_entry, connected_run, connected_exit,
					     NULL, &states[STATE_SYNC_IDLE]),
	[STATE_SYNC_IDLE] = SMF_CREATE_STATE(sync_idle_entry, sync_idle_run, NULL,
					     &states[STATE_CONNECTED], NULL),
	[STATE_SYNCING] = SMF_CREATE_STATE(syncing_entry, syncing_run, NULL,
					   &states[STATE_CONNECTED], NULL),
};

static void telemetry_timer_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct main_msg msg = { .type = MAIN_SYNC };
	int err;

	err = zbus_chan_pub(&main_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub main_chan, error: %d", err);
		FATAL_ERROR();
	}
}

#if defined(CONFIG_APP_BATTERY)
static void sample_timer_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct battery_msg msg = { .type = BATTERY_SAMPLE };
	int err;

	err = zbus_chan_pub(&battery_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub battery_chan, error: %d", err);
		FATAL_ERROR();
	}

	err = k_work_reschedule(&sample_timer, K_SECONDS(CONFIG_APP_BATTERY_SAMPLE_INTERVAL));
	if (err < 0) {
		LOG_ERR("k_work_reschedule sample_timer, error: %d", err);
		FATAL_ERROR();
	}
}
#endif

static void sync_timer_arm(k_timeout_t delay)
{
	int err;

	err = k_work_reschedule(&telemetry_timer, delay);
	if (err < 0) {
		LOG_ERR("k_work_reschedule telemetry_timer, error: %d", err);
		FATAL_ERROR();
	}
}

/* Returns the number of synchronization steps requested. Each one reports back when done. */
static uint8_t report_telemetry(void)
{
	uint8_t requested = 0;
	int err;

	LOG_DBG("Reporting telemetry");
#if defined(CONFIG_APP_LOCATION)
	struct location_msg loc_msg = { .type = LOCATION_FIX_REQUEST };

	err = zbus_chan_pub(&location_chan, &loc_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub location_chan, error: %d", err);
		FATAL_ERROR();
	} else {
		requested++;
	}
#endif
#if defined(CONFIG_APP_BATTERY)
	struct cloud_msg cloud_msg = { .type = CLOUD_BATTERY_SAMPLE };

	err = battery_percent_get(&cloud_msg.battery_percent);
	if (err) {
		LOG_WRN("battery_percent_get, error: %d", err);
	} else {
		err = zbus_chan_pub(&cloud_chan, &cloud_msg, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("zbus_chan_pub cloud_chan, error: %d", err);
			FATAL_ERROR();
		} else {
			requested++;
		}
	}
#endif

	(void)err;

	return requested;
}

static bool is_sync_module_chan(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);

#if defined(CONFIG_APP_LOCATION)
	if (chan == &location_chan) {
		return true;
	}
#endif
#if defined(CONFIG_APP_BATTERY)
	if (chan == &cloud_chan) {
		return true;
	}
#endif

	return false;
}

static bool is_sync_step_done(const struct app_object *state)
{
	ARG_UNUSED(state);

#if defined(CONFIG_APP_LOCATION)
	if (state->chan == &location_chan) {
		return ((const struct location_msg *)state->msg_buf)->type == LOCATION_FIX_DONE;
	}
#endif
#if defined(CONFIG_APP_BATTERY)
	if (state->chan == &cloud_chan) {
		return ((const struct cloud_msg *)state->msg_buf)->type == CLOUD_BATTERY_DONE;
	}
#endif

	return false;
}

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
#endif

#if defined(CONFIG_APP_BUTTONS)
/* A non-NULL skip_reason tells why a sync cannot start right now. */
static void handle_button(const struct button_msg *msg, const char *skip_reason)
{
	int err;

	if (msg->type != BUTTON_1) {
		LOG_WRN("Button %u: no action defined", msg->type);

		return;
	}

	if (skip_reason) {
		LOG_INF("Button 1: %s, sync skipped", skip_reason);

		return;
	}

	LOG_INF("Button 1: sync requested");

	err = k_work_reschedule(&telemetry_timer, K_NO_WAIT);
	if (err < 0) {
		LOG_ERR("k_work_reschedule telemetry_timer, error: %d", err);
	}
}
#endif

static void disconnected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("%s", __func__);
	LOG_INF("Network disconnected");

#if defined(CONFIG_APP_LED)
	led_pattern_set(&led_pattern_disconnected);
#endif
}

static enum smf_state_result disconnected_run(void *obj)
{
	struct app_object *state = obj;

#if defined(CONFIG_APP_BUTTONS)
	if (state->chan == &button_chan) {
		handle_button((const struct button_msg *)state->msg_buf, "network is down");

		return SMF_EVENT_HANDLED;
	}
#endif

	if (state->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			smf_set_state(SMF_CTX(state), &states[STATE_CONNECTED]);
		}
	} else if (state->chan == &main_chan) {
		LOG_WRN("Sync requested while the network is down, ignoring");
	} else if (!is_sync_module_chan(state->chan)) {
		LOG_WRN("Unhandled message in disconnected state.");
	}

	return SMF_EVENT_HANDLED;
}

static void connected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("%s", __func__);
	LOG_INF("Network connected, first sync in %d s", CONFIG_APP_SYNC_BOOT_DELAY_SECONDS);

	sync_timer_arm(K_SECONDS(CONFIG_APP_SYNC_BOOT_DELAY_SECONDS));
}

static enum smf_state_result connected_run(void *obj)
{
	struct app_object *state = obj;

	if (state->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state->msg_buf;

		if (msg->type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state), &states[STATE_DISCONNECTED]);
		}
#if defined(CONFIG_APP_BUTTONS)
	} else if (state->chan == &button_chan) {
		handle_button((const struct button_msg *)state->msg_buf, NULL);
#endif
	} else if (!is_sync_module_chan(state->chan)) {
		LOG_WRN("Unhandled message in connected state.");
	}

	return SMF_EVENT_HANDLED;
}

static void connected_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("%s", __func__);

	(void)k_work_cancel_delayable(&telemetry_timer);
}

static void sync_idle_entry(void *obj)
{
	ARG_UNUSED(obj);

#if defined(CONFIG_APP_LED)
	led_pattern_set(&led_pattern_idle);
#endif
}

static enum smf_state_result sync_idle_run(void *obj)
{
	struct app_object *state = obj;

	if (state->chan == &main_chan) {
		const struct main_msg *msg = (const struct main_msg *)state->msg_buf;

		if (msg->type == MAIN_SYNC) {
			LOG_DBG("Sync tick, next in %d s", CONFIG_APP_SYNC_INTERVAL);

			state->sync_pending = report_telemetry();
			sync_timer_arm(K_SECONDS(CONFIG_APP_SYNC_INTERVAL));

			if (state->sync_pending > 0) {
				smf_set_state(SMF_CTX(state), &states[STATE_SYNCING]);
			}

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void syncing_entry(void *obj)
{
	ARG_UNUSED(obj);

#if defined(CONFIG_APP_LED)
	led_pattern_set(&led_pattern_sync);
#endif
}

static enum smf_state_result syncing_run(void *obj)
{
	struct app_object *state = obj;

	if (state->chan == &main_chan) {
		LOG_WRN("Sync requested while a sync is running, ignoring");
		sync_timer_arm(K_SECONDS(CONFIG_APP_SYNC_INTERVAL));

		return SMF_EVENT_HANDLED;
	}

#if defined(CONFIG_APP_BUTTONS)
	if (state->chan == &button_chan) {
		handle_button((const struct button_msg *)state->msg_buf, "sync is running");

		return SMF_EVENT_HANDLED;
	}
#endif

	if (is_sync_step_done(state)) {
		state->sync_pending--;

		if (state->sync_pending == 0) {
			smf_set_state(SMF_CTX(state), &states[STATE_SYNC_IDLE]);
		}

		return SMF_EVENT_HANDLED;
	}

	return SMF_EVENT_PROPAGATE;
}

static void wdt_callback(int channel_id, void *user_data)
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
	static struct app_object app;

	err = task_wdt_init(DEVICE_DT_GET(DT_ALIAS(watchdog0)));
	if (err) {
		LOG_ERR("task_wdt_init, error: %d", err);
		FATAL_ERROR();
		return -EFAULT;
	}

	task_wdt_id = task_wdt_add(wdt_timeout_ms, wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("task_wdt_add, error: %d", task_wdt_id);
		FATAL_ERROR();
		return -EFAULT;
	}

#if defined(CONFIG_APP_BATTERY)
	err = k_work_reschedule(&sample_timer, K_NO_WAIT);
	if (err < 0) {
		LOG_ERR("k_work_reschedule sample_timer, error: %d", err);
		FATAL_ERROR();
		return -EFAULT;
	}
#endif

	smf_set_initial(SMF_CTX(&app), &states[STATE_DISCONNECTED]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			FATAL_ERROR();
			return -EFAULT;
		}

		err = zbus_sub_wait_msg(&main_subscriber, &app.chan, app.msg_buf, zbus_wait);
		if (err == -ENOMSG) {
			continue;
		}

		if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			FATAL_ERROR();
			return -EFAULT;
		}

		err = smf_run_state(SMF_CTX(&app));
		if (err) {
			LOG_ERR("smf_run_state, error: %d", err);
			FATAL_ERROR();
			return -EFAULT;
		}
	}

	return 0;
}
