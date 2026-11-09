/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "app_common.h"
#include "modules/modem_at/modem_at.h"
#include "location.h"

LOG_MODULE_REGISTER(location, CONFIG_APP_LOCATION_LOG_LEVEL);

/* Slack for the Serial Modem to report the GNSS result after its own timeout. */
#define GNSS_RESULT_MARGIN_SECONDS 10

BUILD_ASSERT(CONFIG_APP_LOCATION_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must exceed the maximum message processing time");
BUILD_ASSERT(CONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS >
	     (CONFIG_APP_MODEM_AT_WAIT_TIMEOUT_SECONDS + CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS +
	      CONFIG_APP_LOCATION_CELL_TIMEOUT_SECONDS),
	     "Message processing timeout must exceed the AT channel wait plus the cell request");
#if defined(CONFIG_APP_LOCATION_GNSS)
BUILD_ASSERT(CONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS >
	     (CONFIG_APP_MODEM_AT_WAIT_TIMEOUT_SECONDS + CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS +
	      CONFIG_APP_LOCATION_GNSS_TIMEOUT_SECONDS + GNSS_RESULT_MARGIN_SECONDS),
	     "Message processing timeout must exceed the AT channel wait plus the GNSS fix");
#endif

#define XNRFCLOUDPOS_URC	"#XNRFCLOUDPOS: "
#define XGNSSPOS_URC		"#XGNSSPOS: "
#define XGNSS_URC		"#XGNSS: "

/* #XGNSS <gnss_status> values that end a single fix without a position. */
#define XGNSS_STATUS_STOPPED	0
#define XGNSS_STATUS_TIMEOUT	3

#define URC_BUF_SIZE 96

ZBUS_CHAN_DEFINE(location_chan,
		 struct location_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

#if defined(CONFIG_APP_LOCATION_GNSS)
enum priv_location_msg_type {
	/* The cellular request failed, try GNSS. */
	LOCATION_PRIV_GNSS_FALLBACK,
};

struct priv_location_msg {
	enum priv_location_msg_type type;
};

/* The GNSS fallback is its own message, so the task watchdog is fed between the attempts. */
ZBUS_CHAN_DEFINE(location_priv_chan,
		 struct priv_location_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
#endif /* CONFIG_APP_LOCATION_GNSS */

ZBUS_MSG_SUBSCRIBER_DEFINE(location);

#define CHANNEL_LIST(X)							\
	X(location_chan, struct location_msg)				\
	IF_ENABLED(CONFIG_APP_LOCATION_GNSS,				\
		   (X(location_priv_chan, struct priv_location_msg)))

#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, location, 0);

CHANNEL_LIST(ADD_OBSERVERS)

#if defined(CONFIG_APP_LOCATION_GNSS)
/* Result of the running GNSS fix, filled in from the modem receive context. */
static struct {
	struct k_spinlock lock;
	struct k_sem done;
	bool active;
	bool fix;
	char pos[URC_BUF_SIZE];
} gnss;
#endif

/* Split comma-separated URC arguments in place. Returns the number of fields. */
static size_t split_args(char *buf, char **fields, size_t max_fields)
{
	size_t count = 0;
	char *save;

	char *tok = strtok_r(buf, ",", &save);

	while (tok != NULL && count < max_fields) {
		fields[count++] = tok;
		tok = strtok_r(NULL, ",", &save);
	}

	return count;
}

static const char *cell_method_str(const char *type)
{
	switch (atoi(type)) {
	case 0: return "Single-cell";
	case 1: return "Multicell";
	case 2: return "Wi-Fi";
	default: return type;
	}
}

/* #XNRFCLOUDPOS: <status>[,<type>,<latitude>,<longitude>,<uncertainty>] */
static int cell_location_request(void)
{
	char cmd[32];
	char urc[URC_BUF_SIZE];
	char *fields[5];
	size_t count;
	int err;

	err = snprintk(cmd, sizeof(cmd), "AT#XNRFCLOUDPOS=%d,0", CONFIG_APP_LOCATION_CELL_COUNT);
	if (err < 0 || err >= (int)sizeof(cmd)) {
		LOG_ERR("snprintk, error: %d", err);
		return -EINVAL;
	}

	LOG_DBG("Requesting cellular location, cells: %d", CONFIG_APP_LOCATION_CELL_COUNT);

	err = modem_at_run_wait_urc(cmd, XNRFCLOUDPOS_URC, urc, sizeof(urc),
				    CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS,
				    CONFIG_APP_LOCATION_CELL_TIMEOUT_SECONDS);
	if (err) {
		LOG_WRN("Cellular location request failed, error: %d", err);
		return err;
	}

	count = split_args(urc, fields, ARRAY_SIZE(fields));
	if (count < 1 || atoi(fields[0]) != 0) {
		LOG_WRN("Cellular location failed, result: %s", (count > 0) ? fields[0] : "");
		return -ENODATA;
	}

	if (count < 5) {
		LOG_WRN("Unexpected #XNRFCLOUDPOS result format");
		return -EBADMSG;
	}

	LOG_INF("Location: %s,%s Uncertainty: %sm Method: %s",
		fields[2], fields[3], fields[4], cell_method_str(fields[1]));
	LOG_DBG("Google maps URL: https://maps.google.com/?q=%s,%s", fields[2], fields[3]);

	return 0;
}

#if defined(CONFIG_APP_LOCATION_GNSS)
/* Handle the #XGNSSPOS URC: latitude, longitude, altitude, accuracy, speed, heading, datetime. */
static void on_gnss_pos(char **argv, uint16_t argc, void *user_data)
{
	ARG_UNUSED(user_data);

	bool done = false;
	k_spinlock_key_t key;

	if (argc < 5) {
		return;
	}

	key = k_spin_lock(&gnss.lock);
	if (gnss.active) {
		snprintk(gnss.pos, sizeof(gnss.pos), "%s,%s,%s", argv[1], argv[2], argv[4]);
		gnss.fix = true;
		gnss.active = false;
		done = true;
	}
	k_spin_unlock(&gnss.lock, key);

	if (done) {
		k_sem_give(&gnss.done);
	}
}

/* Handle the #XGNSS URC: GNSS service and GNSS status. */
static void on_gnss_status(char **argv, uint16_t argc, void *user_data)
{
	ARG_UNUSED(user_data);

	bool done = false;
	int status;
	k_spinlock_key_t key;

	if (argc < 3) {
		return;
	}

	status = atoi(argv[2]);
	if (status != XGNSS_STATUS_TIMEOUT && status != XGNSS_STATUS_STOPPED) {
		return;
	}

	key = k_spin_lock(&gnss.lock);
	if (gnss.active) {
		gnss.active = false;
		done = true;
	}
	k_spin_unlock(&gnss.lock, key);

	if (done) {
		k_sem_give(&gnss.done);
	}
}

static void gnss_antenna_configure(void)
{
	static bool configured;
	int err;

	if (configured || sizeof(CONFIG_APP_LOCATION_GNSS_ANTENNA_CMD) <= 1) {
		return;
	}

	err = modem_at_run(CONFIG_APP_LOCATION_GNSS_ANTENNA_CMD, NULL, 0,
			   CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS);
	if (err) {
		LOG_WRN("%s, error: %d", CONFIG_APP_LOCATION_GNSS_ANTENNA_CMD, err);
		return;
	}

	configured = true;
}

/* Single GNSS fix with A-GNSS. Serial Modem sends the fix to nRF Cloud itself, because
 * the cloud module connects with <send_location> set.
 */
static int gnss_location_request(void)
{
	char cmd[32];
	char *fields[3];
	k_spinlock_key_t key;
	bool fix;
	int err;

	err = snprintk(cmd, sizeof(cmd), "AT#XGNSS=1,1,0,%d",
		       CONFIG_APP_LOCATION_GNSS_TIMEOUT_SECONDS);
	if (err < 0 || err >= (int)sizeof(cmd)) {
		LOG_ERR("snprintk, error: %d", err);
		return -EINVAL;
	}

	gnss_antenna_configure();

	key = k_spin_lock(&gnss.lock);
	gnss.active = true;
	gnss.fix = false;
	k_sem_reset(&gnss.done);
	k_spin_unlock(&gnss.lock, key);

	LOG_DBG("Starting GNSS, timeout: %d s", CONFIG_APP_LOCATION_GNSS_TIMEOUT_SECONDS);

	err = modem_at_run(cmd, NULL, 0, CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS);
	if (!err) {
		err = k_sem_take(&gnss.done, K_SECONDS(CONFIG_APP_LOCATION_GNSS_TIMEOUT_SECONDS +
							GNSS_RESULT_MARGIN_SECONDS));
	}

	key = k_spin_lock(&gnss.lock);
	gnss.active = false;
	fix = gnss.fix;
	k_spin_unlock(&gnss.lock, key);

	if (err) {
		LOG_WRN("GNSS failed, error: %d", err);
		(void)modem_at_run("AT#XGNSS=0", NULL, 0, CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS);
		return err;
	}

	if (!fix) {
		LOG_WRN("GNSS timed out without a fix");
		return -ENODATA;
	}

	if (split_args(gnss.pos, fields, ARRAY_SIZE(fields)) < ARRAY_SIZE(fields)) {
		LOG_WRN("Unexpected #XGNSSPOS format");
		return -EBADMSG;
	}

	LOG_INF("Location: %s,%s Uncertainty: %sm Method: GNSS", fields[0], fields[1], fields[2]);
	LOG_DBG("Google maps URL: https://maps.google.com/?q=%s,%s", fields[0], fields[1]);

	return 0;
}
#endif /* CONFIG_APP_LOCATION_GNSS */

static void location_fix_done(void)
{
	struct location_msg msg = { .type = LOCATION_FIX_DONE };
	int err;

	err = zbus_chan_pub(&location_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub location_chan, error: %d", err);
		FATAL_ERROR();
	}
}

#if defined(CONFIG_APP_LOCATION_GNSS)
static void location_gnss_fallback(void)
{
	struct priv_location_msg msg = { .type = LOCATION_PRIV_GNSS_FALLBACK };
	int err;

	err = zbus_chan_pub(&location_priv_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub location_priv_chan, error: %d", err);
		FATAL_ERROR();
	}
}
#endif /* CONFIG_APP_LOCATION_GNSS */

static void on_fix_request(void)
{
	LOG_DBG("Location fix request received");

	if (cell_location_request() == 0) {
		location_fix_done();
		return;
	}

#if defined(CONFIG_APP_LOCATION_GNSS)
	location_gnss_fallback();
#else
	LOG_WRN("No location this sync");
	location_fix_done();
#endif /* CONFIG_APP_LOCATION_GNSS */
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

#if defined(CONFIG_APP_LOCATION_GNSS)
	k_sem_init(&gnss.done, 0, 1);

	err = modem_at_urc_subscribe(XGNSSPOS_URC, on_gnss_pos, NULL);
	if (err) {
		LOG_ERR("Failed to subscribe to #XGNSSPOS URC: %d", err);
		FATAL_ERROR();
	}

	err = modem_at_urc_subscribe(XGNSS_URC, on_gnss_status, NULL);
	if (err) {
		LOG_ERR("Failed to subscribe to #XGNSS URC: %d", err);
		FATAL_ERROR();
	}
#endif

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
				on_fix_request();
			}
#if defined(CONFIG_APP_LOCATION_GNSS)
		} else if (chan == &location_priv_chan) {
			const struct priv_location_msg *msg =
				(const struct priv_location_msg *)msg_buf;

			if (msg->type == LOCATION_PRIV_GNSS_FALLBACK) {
				if (gnss_location_request()) {
					LOG_WRN("No location this sync");
				}

				location_fix_done();
			}
#endif /* CONFIG_APP_LOCATION_GNSS */
		} else {
			LOG_WRN("Unhandled message in location thread");
		}
	}
}

K_THREAD_DEFINE(location_tid, CONFIG_APP_LOCATION_THREAD_STACK_SIZE, location_thread,
		NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
