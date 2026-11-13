/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/modem/at/user_pipe.h>
#include <zephyr/modem/chat.h>

#include "app_common.h"
#include "modem_at.h"

LOG_MODULE_REGISTER(modem_at, CONFIG_APP_MODEM_AT_LOG_LEVEL);

struct modem_at_ctx {
	struct modem_chat chat;
	uint8_t chat_receive_buf[CONFIG_APP_MODEM_AT_CHAT_RECEIVE_BUF_SIZE];
	uint8_t *chat_argv[8];
	struct modem_chat_script_chat script_chat[1];
	struct modem_chat_match script_matches[2];
	struct modem_chat_script script;
	uint8_t request_buf[CONFIG_APP_MODEM_AT_COMMAND_MAX_SIZE];
	struct k_mutex run_lock;
	char *collect_buf;
	size_t collect_size;
	size_t collect_len;
};

static struct modem_at_ctx at_ctx;

static void collect_line(struct modem_chat *c, char **argv, uint16_t argc, void *user_data);
static void on_error_line(struct modem_chat *c, char **argv, uint16_t argc, void *user_data);
static void init_script_chat(void);
static int modem_at_init(void);

MODEM_CHAT_MATCHES_DEFINE(script_abort_matches,
	MODEM_CHAT_MATCH("ERROR", "", NULL),
	MODEM_CHAT_MATCH("+CME ERROR:", "", on_error_line),
	MODEM_CHAT_MATCH("+CMS ERROR:", "", on_error_line));

static void collect_line(struct modem_chat *c, char **argv, uint16_t argc, void *user_data)
{
	ARG_UNUSED(c);
	ARG_UNUSED(user_data);

	if (argc != 2 || at_ctx.collect_buf == NULL || at_ctx.collect_size == 0) {
		return;
	}

	size_t len = strnlen(argv[1], at_ctx.collect_size);

	if (len + 2 > at_ctx.collect_size - at_ctx.collect_len) {
		return;
	}

	memcpy(&at_ctx.collect_buf[at_ctx.collect_len], argv[1], len);
	at_ctx.collect_len += len;
	at_ctx.collect_buf[at_ctx.collect_len++] = '\n';
	at_ctx.collect_buf[at_ctx.collect_len] = '\0';
}

static void on_error_line(struct modem_chat *c, char **argv, uint16_t argc, void *user_data)
{
	ARG_UNUSED(c);
	ARG_UNUSED(user_data);

	if (at_ctx.collect_buf == NULL || at_ctx.collect_size == 0 || argc < 1) {
		return;
	}

	int ret = snprintk(at_ctx.collect_buf, at_ctx.collect_size,
			   "%s%s", argv[0],
			   (argc > 1 && argv[1] != NULL) ? argv[1] : "");

	if (ret < 0) {
		at_ctx.collect_len = 0;
		at_ctx.collect_buf[0] = '\0';
	} else if ((size_t)ret >= at_ctx.collect_size) {
		at_ctx.collect_len = at_ctx.collect_size - 1;
	} else {
		at_ctx.collect_len = (size_t)ret;
	}
}

static void init_script_chat(void)
{
	modem_chat_match_init(&at_ctx.script_matches[0]);
	modem_chat_match_set_match(&at_ctx.script_matches[0], "");
	modem_chat_match_set_separators(&at_ctx.script_matches[0], "");
	modem_chat_match_set_callback(&at_ctx.script_matches[0], collect_line);
	modem_chat_match_set_partial(&at_ctx.script_matches[0], true);
	modem_chat_match_enable_wildcards(&at_ctx.script_matches[0], false);

	modem_chat_match_init(&at_ctx.script_matches[1]);
	modem_chat_match_set_match(&at_ctx.script_matches[1], "OK");
	modem_chat_match_set_separators(&at_ctx.script_matches[1], "");
	modem_chat_match_set_partial(&at_ctx.script_matches[1], false);
	modem_chat_match_enable_wildcards(&at_ctx.script_matches[1], false);

	modem_chat_script_chat_init(&at_ctx.script_chat[0]);
	modem_chat_script_chat_set_response_matches(&at_ctx.script_chat[0],
						    at_ctx.script_matches,
						    ARRAY_SIZE(at_ctx.script_matches));

	at_ctx.script.name = "at_script";
	at_ctx.script.script_chats = at_ctx.script_chat;
	at_ctx.script.script_chats_size = ARRAY_SIZE(at_ctx.script_chat);
	at_ctx.script.abort_matches = script_abort_matches;
	at_ctx.script.abort_matches_size = ARRAY_SIZE(script_abort_matches);
	at_ctx.script.callback = NULL;
	at_ctx.script.timeout = CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS;
}

int modem_at_run(const char *req, char *resp, size_t resp_size, uint32_t timeout_s)
{
	int ret;

	if (req == NULL) {
		return -EINVAL;
	}

	ret = k_mutex_lock(&at_ctx.run_lock,
			   K_SECONDS(CONFIG_APP_MODEM_AT_PIPE_WAIT_TIMEOUT_SECONDS));
	if (ret) {
		LOG_WRN("Timed out waiting for AT pipe after %us",
			CONFIG_APP_MODEM_AT_PIPE_WAIT_TIMEOUT_SECONDS);
		return ret;
	}

	ret = modem_at_user_pipe_claim(&at_ctx.chat,
				       K_SECONDS(CONFIG_APP_MODEM_AT_PIPE_WAIT_TIMEOUT_SECONDS));
	if (ret < 0) {
		k_mutex_unlock(&at_ctx.run_lock);
		return ret;
	}

	at_ctx.collect_buf = resp;
	at_ctx.collect_size = resp_size;
	at_ctx.collect_len = 0;
	if (resp != NULL && resp_size > 0) {
		resp[0] = '\0';
	}

	ret = snprintk((char *)at_ctx.request_buf, sizeof(at_ctx.request_buf), "%s", req);
	if (ret < 0 || ret >= (int)sizeof(at_ctx.request_buf)) {
		ret = -EINVAL;
		goto release;
	}

	ret = modem_chat_script_chat_set_request(&at_ctx.script_chat[0], at_ctx.request_buf);
	if (ret < 0) {
		ret = -EINVAL;
		goto release;
	}

	modem_chat_script_set_timeout(&at_ctx.script, timeout_s);

	LOG_DBG("AT> %s (timeout %us)", req, timeout_s);

	ret = modem_chat_run_script(&at_ctx.chat, &at_ctx.script);
	if (ret == 0 && at_ctx.chat.script_result != MODEM_CHAT_SCRIPT_RESULT_SUCCESS) {
		ret = -EIO;
	}

	if (ret) {
		LOG_DBG("AT command failed, error: %d, script result: %d", ret,
			at_ctx.chat.script_result);
	} else if (resp != NULL && resp_size > 0) {
		LOG_DBG("AT< %s", resp);
	} else {
		LOG_DBG("AT command OK");
	}

release:
	at_ctx.collect_buf = NULL;
	modem_at_user_pipe_release();
	k_mutex_unlock(&at_ctx.run_lock);
	return ret;
}

static int modem_at_init(void)
{
	int err;
	const struct modem_chat_config chat_config = {
		.receive_buf = at_ctx.chat_receive_buf,
		.receive_buf_size = sizeof(at_ctx.chat_receive_buf),
		.delimiter = "\r",
		.delimiter_size = sizeof("\r") - 1,
		.filter = "\n",
		.filter_size = sizeof("\n") - 1,
		.argv = at_ctx.chat_argv,
		.argv_size = ARRAY_SIZE(at_ctx.chat_argv),
	};

	k_mutex_init(&at_ctx.run_lock);

	err = modem_chat_init(&at_ctx.chat, &chat_config);
	if (err) {
		LOG_ERR("modem_chat_init, error: %d", err);
		FATAL_ERROR();
		return err;
	}

	init_script_chat();

	return 0;
}

SYS_INIT(modem_at_init, POST_KERNEL, 99);
