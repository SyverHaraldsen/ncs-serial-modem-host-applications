/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "stream.h"
#if defined(CONFIG_APP_DETECTION)
#include "modules/camera/camera.h"
#include "modules/detection/detection.h"
#endif

static int publish(const struct shell *sh, const struct stream_msg *msg)
{
	int err = zbus_chan_pub(&stream_chan, msg, PUB_TIMEOUT);

	if (err) {
		shell_error(sh, "zbus_chan_pub stream_chan, error: %d", err);
	}

	return err;
}

static int cmd_stream_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return publish(sh, &(struct stream_msg){ .type = STREAM_START });
}

static int cmd_stream_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return publish(sh, &(struct stream_msg){ .type = STREAM_STOP });
}

static int cmd_stream_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return publish(sh, &(struct stream_msg){ .type = STREAM_STATUS });
}

static int cmd_stream_endpoint(const struct shell *sh, size_t argc, char **argv)
{
	struct stream_msg msg = { .type = STREAM_ENDPOINT, .port = CONFIG_APP_STREAM_PORT };

	if (strlen(argv[1]) > STREAM_HOST_MAX_LEN) {
		shell_error(sh, "Host is longer than %d characters", STREAM_HOST_MAX_LEN);
		return -EINVAL;
	}

	if (argc > 2) {
		char *end;
		unsigned long port = strtoul(argv[2], &end, 10);

		if (*end != '\0' || port == 0 || port > UINT16_MAX) {
			shell_error(sh, "Invalid port: %s", argv[2]);
			return -EINVAL;
		}
		msg.port = (uint16_t)port;
	}

	strcpy(msg.host, argv[1]);

	return publish(sh, &msg);
}

#if defined(CONFIG_APP_DETECTION)
/* The link only has room for one of the two, so selecting one turns the other off. */
static int source_select(const struct shell *sh, bool ai_input)
{
	int err;

	err = zbus_chan_pub(&detection_chan, &(struct detection_msg){
		.type = DETECTION_AI_INPUT_SET,
		.ai_input = ai_input,
	}, PUB_TIMEOUT);
	if (!err) {
		err = zbus_chan_pub(&camera_chan, &(struct camera_msg){
			.type = CAMERA_STREAM_SET,
			.stream = !ai_input,
		}, PUB_TIMEOUT);
	}
	if (err) {
		shell_error(sh, "zbus_chan_pub, error: %d", err);
	}

	return err;
}

static int cmd_stream_video(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return source_select(sh, false);
}

static int cmd_stream_ai_input(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return source_select(sh, true);
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(stream_cmds,
	SHELL_CMD(start, NULL, "Stream whenever the network is up.", cmd_stream_start),
	SHELL_CMD(stop, NULL, "Stop streaming until 'stream start' or reboot.", cmd_stream_stop),
	SHELL_CMD_ARG(endpoint, NULL, "Set and store the endpoint: <host> [port]",
		      cmd_stream_endpoint, 2, 1),
	SHELL_CMD(status, NULL, "Log the stream state, endpoint and data sent.",
		  cmd_stream_status),
	IF_ENABLED(CONFIG_APP_DETECTION, (
	SHELL_CMD(video, NULL, "Stream the camera video with detection boxes. The default.",
		  cmd_stream_video),
	SHELL_CMD(ai-input, NULL, "Stream what the AI is fed (160x128) with its boxes.",
		  cmd_stream_ai_input),
	))
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(stream, &stream_cmds, "UDP video stream control", NULL);
