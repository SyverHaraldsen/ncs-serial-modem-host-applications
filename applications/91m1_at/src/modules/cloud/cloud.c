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
#include "cloud.h"

LOG_MODULE_REGISTER(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must exceed the maximum message processing time");
BUILD_ASSERT(CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS >
	     (CONFIG_APP_MODEM_AT_WAIT_TIMEOUT_SECONDS +
	      CONFIG_APP_CLOUD_AT_TIMEOUT_SECONDS +
	      CONFIG_APP_CLOUD_PROVISIONING_STEP_TIMEOUT_SECONDS),
	     "Message processing timeout must exceed the AT channel wait plus the provisioning step");
BUILD_ASSERT(CONFIG_APP_CLOUD_PROVISIONING_STEP_TIMEOUT_SECONDS >=
	     CONFIG_APP_CLOUD_CONNECT_TIMEOUT_SECONDS,
	     "The provisioning step is the longest exchange the processing timeout covers");

/* AT#XNRFPROV result codes (Serial Modem sm_at_provisioning.c). */
#define XNRFPROV_DONE			0
#define XNRFPROV_NEED_LTE_DEACTIVATED	1
#define XNRFPROV_NEED_LTE_ACTIVATED	2
#define XNRFPROV_FAILED			-1
#define XNRFPROV_FAILED_NOT_CLAIMED	-2

/* Connect with <send_location> set, so the Serial Modem sends each GNSS fix to nRF Cloud. */
#define CONNECT_CMD "AT#XNRFCLOUD=1,1"

#define XNRFPROV_URC	"#XNRFPROV: "
#define XNRFCLOUD_URC	"#XNRFCLOUD: "

#define UUID_MAX_LEN		40
#define ATTEST_TOKEN_MAX_LEN	512
#define URC_BUF_SIZE		32

ZBUS_CHAN_DEFINE(cloud_chan,
		 struct cloud_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

enum priv_msg_type {
	/* Retry timer expired. */
	PRIV_RETRY,
	/* Start a provisioning session. */
	PRIV_PROVISION_START,
	/* Result of a provisioning step, value in .status. */
	PRIV_PROVISION_STATUS,
	/* Connect the modem's nRF Cloud client. */
	PRIV_CONNECT,
};

struct priv_msg {
	enum priv_msg_type type;
	int status;
};

/* Internal triggers. Each provisioning step is its own message, so the task watchdog
 * is fed between the steps of a session.
 */
ZBUS_CHAN_DEFINE(cloud_priv_chan,
		 struct priv_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(cloud);

#define CHANNEL_LIST(X)				\
	X(cloud_chan, struct cloud_msg)		\
	X(cloud_priv_chan, struct priv_msg)

#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, cloud, 0);

CHANNEL_LIST(ADD_OBSERVERS)

enum cloud_state {
	STATE_DISCONNECTED,
	STATE_PROVISIONING,
	STATE_CONNECTING,
	STATE_CONNECTED,
};

struct cloud_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	/* Main has requested a connection, tracked in every state. */
	bool connect_requested;
	/* Provisioning ran to completion since boot. */
	bool provisioned;
	/* The host took LTE offline for the provisioning session. */
	bool lte_offline;
};

static void retry_timer_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(retry_timer, retry_timer_fn);

static void disconnected_entry(void *obj);
static enum smf_state_result disconnected_run(void *obj);
static void provisioning_entry(void *obj);
static enum smf_state_result provisioning_run(void *obj);
static void connecting_entry(void *obj);
static enum smf_state_result connecting_run(void *obj);
static void connected_entry(void *obj);
static enum smf_state_result connected_run(void *obj);
static void connected_exit(void *obj);

static const struct smf_state states[] = {
	[STATE_DISCONNECTED] = SMF_CREATE_STATE(disconnected_entry, disconnected_run,
						NULL, NULL, NULL),
	[STATE_PROVISIONING] = SMF_CREATE_STATE(provisioning_entry, provisioning_run,
						NULL, NULL, NULL),
	[STATE_CONNECTING] = SMF_CREATE_STATE(connecting_entry, connecting_run,
					      NULL, NULL, NULL),
	[STATE_CONNECTED] = SMF_CREATE_STATE(connected_entry, connected_run, connected_exit,
					     NULL, NULL),
};

/* Helper functions */

static void publish_priv(enum priv_msg_type type, int status)
{
	struct priv_msg msg = { .type = type, .status = status };
	int err;

	err = zbus_chan_pub(&cloud_priv_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub cloud_priv_chan, error: %d", err);
		FATAL_ERROR();
	}
}

static void publish_status(enum cloud_msg_type type)
{
	struct cloud_msg msg = { .type = type };
	int err;

	err = zbus_chan_pub(&cloud_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub cloud_chan, error: %d", err);
		FATAL_ERROR();
	}
}

static void retry_timer_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	publish_priv(PRIV_RETRY, 0);
}

static void schedule_retry(void)
{
	int err;

	LOG_INF("Retrying in %d s", CONFIG_APP_CLOUD_RETRY_SECONDS);

	err = k_work_reschedule(&retry_timer, K_SECONDS(CONFIG_APP_CLOUD_RETRY_SECONDS));
	if (err < 0) {
		LOG_ERR("k_work_reschedule retry_timer, error: %d", err);
		FATAL_ERROR();
	}
}

/* Copy the text between the first pair of double quotes in @p src into @p dst. */
static int quoted_value_get(const char *src, char *dst, size_t dst_size)
{
	const char *start = strchr(src, '"');
	const char *end;
	size_t len;

	if (start == NULL) {
		return -EINVAL;
	}

	start++;
	end = strchr(start, '"');
	if (end == NULL) {
		return -EINVAL;
	}

	len = end - start;
	if (len == 0 || len >= dst_size) {
		return -EINVAL;
	}

	memcpy(dst, start, len);
	dst[len] = '\0';

	return 0;
}

/* AT#XNRFCLOUD? -> #XNRFCLOUD: <ready>,<send_location>,<sec_tag>,"<device_id>" */
static int cloud_state_read(bool *ready, char *uuid, size_t uuid_size)
{
	char resp[96];
	const char *args;
	int err;

	err = modem_at_run("AT#XNRFCLOUD?", resp, sizeof(resp),
			   CONFIG_APP_CLOUD_AT_TIMEOUT_SECONDS);
	if (err) {
		return err;
	}

	args = strstr(resp, XNRFCLOUD_URC);
	if (args == NULL) {
		return -EBADMSG;
	}

	args += sizeof(XNRFCLOUD_URC) - 1;
	*ready = (atoi(args) == 1);

	if (uuid != NULL) {
		return quoted_value_get(args, uuid, uuid_size);
	}

	return 0;
}

static void log_device_uuid(void)
{
	static bool logged;
	char uuid[UUID_MAX_LEN];
	bool ready;
	int err;

	if (logged) {
		return;
	}

	err = cloud_state_read(&ready, uuid, sizeof(uuid));
	if (err) {
		LOG_WRN("Failed to read the device UUID, error: %d", err);
		return;
	}

	LOG_INF("Device UUID: %s", uuid);
	logged = true;
}

static void log_attestation_token(void)
{
	/* Only the cloud thread runs this, and the token does not fit its stack. */
	static char resp[ATTEST_TOKEN_MAX_LEN];
	static char token[ATTEST_TOKEN_MAX_LEN];
	int err;

	err = modem_at_run("AT%ATTESTTOKEN", resp, sizeof(resp),
			   CONFIG_APP_CLOUD_AT_TIMEOUT_SECONDS);
	if (err) {
		LOG_WRN("Failed to read the attestation token, error: %d", err);
		return;
	}

	err = quoted_value_get(resp, token, sizeof(token));
	if (err) {
		LOG_WRN("Unexpected attestation token response: %s", resp);
		return;
	}

	LOG_INF("Attestation token: %s", token);
}

/* Run one provisioning step and report its #XNRFPROV result to the state machine. */
static void provision_step(const char *cmd)
{
	char urc[URC_BUF_SIZE];
	int err;

	err = modem_at_run_wait_urc(cmd, XNRFPROV_URC, urc, sizeof(urc),
				    CONFIG_APP_CLOUD_AT_TIMEOUT_SECONDS,
				    CONFIG_APP_CLOUD_PROVISIONING_STEP_TIMEOUT_SECONDS);
	if (err) {
		LOG_WRN("Provisioning step %s failed, error: %d", cmd, err);
		publish_priv(PRIV_PROVISION_STATUS, XNRFPROV_FAILED);
		return;
	}

	publish_priv(PRIV_PROVISION_STATUS, atoi(urc));
}

static void lte_restore(struct cloud_state_object *state)
{
	int err;

	if (!state->lte_offline) {
		return;
	}

	err = modem_at_run("AT+CFUN=1", NULL, 0, CONFIG_APP_CLOUD_AT_TIMEOUT_SECONDS);
	if (err) {
		LOG_ERR("Failed to bring LTE back online, error: %d", err);
		return;
	}

	state->lte_offline = false;
}

/* Zephyr State Machine Framework handlers */

static void disconnected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result disconnected_run(void *obj)
{
	struct cloud_state_object *state = obj;
	bool start = false;

	if (state->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)state->msg_buf;

		start = (msg->type == CLOUD_CONNECT);
	} else if (state->chan == &cloud_priv_chan) {
		const struct priv_msg *msg = (const struct priv_msg *)state->msg_buf;

		start = (msg->type == PRIV_RETRY) && state->connect_requested;
	} else {
	}

	if (start) {
		(void)k_work_cancel_delayable(&retry_timer);
		smf_set_state(SMF_CTX(state), state->provisioned ? &states[STATE_CONNECTING]
								  : &states[STATE_PROVISIONING]);
	}

	return SMF_EVENT_HANDLED;
}

static void provisioning_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	publish_priv(PRIV_PROVISION_START, 0);
}

static void provisioning_status_handle(struct cloud_state_object *state, int status)
{
	switch (status) {
	case XNRFPROV_NEED_LTE_DEACTIVATED:
		LOG_INF("Provisioning: taking LTE offline to store credentials");
		state->lte_offline = true;
		provision_step("AT+CFUN=4");
		break;
	case XNRFPROV_NEED_LTE_ACTIVATED:
		LOG_INF("Provisioning: bringing LTE back online");
		state->lte_offline = false;
		provision_step("AT+CFUN=1");
		break;
	case XNRFPROV_DONE:
		LOG_INF("Provisioning done");
		state->provisioned = true;
		smf_set_state(SMF_CTX(state), state->connect_requested
					      ? &states[STATE_CONNECTING]
					      : &states[STATE_DISCONNECTED]);
		break;
	case XNRFPROV_FAILED_NOT_CLAIMED:
		LOG_WRN("Device not claimed, claim it in nRF Cloud with the attestation token");
		log_attestation_token();
		schedule_retry();
		smf_set_state(SMF_CTX(state), &states[STATE_DISCONNECTED]);
		break;
	default:
		LOG_WRN("Provisioning failed, result: %d", status);
		lte_restore(state);
		schedule_retry();
		smf_set_state(SMF_CTX(state), &states[STATE_DISCONNECTED]);
		break;
	}
}

static enum smf_state_result provisioning_run(void *obj)
{
	struct cloud_state_object *state = obj;
	const struct priv_msg *msg = (const struct priv_msg *)state->msg_buf;

	if (state->chan != &cloud_priv_chan) {
		/* Connect requests during the session are tracked by the thread; the modem
		 * reports through #XNRFPROV when it needs the link.
		 */
		return SMF_EVENT_HANDLED;
	}

	switch (msg->type) {
	case PRIV_PROVISION_START:
		log_device_uuid();
		LOG_INF("Provisioning started");
		provision_step("AT#XNRFPROV");
		break;
	case PRIV_PROVISION_STATUS:
		provisioning_status_handle(state, msg->status);
		break;
	default:
		break;
	}

	return SMF_EVENT_HANDLED;
}

static void connecting_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	publish_priv(PRIV_CONNECT, 0);
}

static enum smf_state_result connecting_run(void *obj)
{
	struct cloud_state_object *state = obj;
	const struct priv_msg *msg = (const struct priv_msg *)state->msg_buf;
	char urc[URC_BUF_SIZE] = "";
	bool ready = false;
	int err;

	if (state->chan == &cloud_chan) {
		if (!state->connect_requested) {
			smf_set_state(SMF_CTX(state), &states[STATE_DISCONNECTED]);
		}

		return SMF_EVENT_HANDLED;
	}

	if (state->chan != &cloud_priv_chan || msg->type != PRIV_CONNECT) {
		return SMF_EVENT_HANDLED;
	}

	/* The modem keeps its nRF Cloud session across a short network outage. */
	err = cloud_state_read(&ready, NULL, 0);
	if (!err && ready) {
		smf_set_state(SMF_CTX(state), &states[STATE_CONNECTED]);
		return SMF_EVENT_HANDLED;
	}

	err = modem_at_run_wait_urc(CONNECT_CMD, XNRFCLOUD_URC, urc, sizeof(urc),
				    CONFIG_APP_CLOUD_AT_TIMEOUT_SECONDS,
				    CONFIG_APP_CLOUD_CONNECT_TIMEOUT_SECONDS);
	if (!err && atoi(urc) == 1) {
		smf_set_state(SMF_CTX(state), &states[STATE_CONNECTED]);
		return SMF_EVENT_HANDLED;
	}

	LOG_WRN("Failed to connect to nRF Cloud, error: %d, result: %s", err, urc);
	schedule_retry();
	smf_set_state(SMF_CTX(state), &states[STATE_DISCONNECTED]);

	return SMF_EVENT_HANDLED;
}

static void connected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	publish_status(CLOUD_CONNECTED);
}

static enum smf_state_result connected_run(void *obj)
{
	struct cloud_state_object *state = obj;

	if (state->chan == &cloud_chan && !state->connect_requested) {
		smf_set_state(SMF_CTX(state), &states[STATE_DISCONNECTED]);
	}

	return SMF_EVENT_HANDLED;
}

static void connected_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	publish_status(CLOUD_DISCONNECTED);
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
	static struct cloud_state_object cloud_state;
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

	smf_set_initial(SMF_CTX(&cloud_state), &states[STATE_DISCONNECTED]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			FATAL_ERROR();
		}

		err = zbus_sub_wait_msg(&cloud, &cloud_state.chan, cloud_state.msg_buf, zbus_wait);
		if (err == -ENOMSG) {
			continue;
		}
		if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			FATAL_ERROR();
		}

		if (cloud_state.chan == &cloud_chan) {
			const struct cloud_msg *msg = (const struct cloud_msg *)cloud_state.msg_buf;

			if (msg->type == CLOUD_CONNECT) {
				cloud_state.connect_requested = true;
			} else if (msg->type == CLOUD_DISCONNECT) {
				cloud_state.connect_requested = false;
			} else {
				/* Own status messages. */
				continue;
			}
		}

		err = smf_run_state(SMF_CTX(&cloud_state));
		if (err) {
			LOG_ERR("smf_run_state, error: %d", err);
			FATAL_ERROR();
		}
	}
}

K_THREAD_DEFINE(cloud_tid, CONFIG_APP_CLOUD_THREAD_STACK_SIZE, cloud_thread,
		NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
