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
#include "detection.h"

static int publish(const struct shell *sh, const struct detection_msg *msg)
{
	int err = zbus_chan_pub(&detection_chan, msg, PUB_TIMEOUT);

	if (err) {
		shell_error(sh, "zbus_chan_pub detection_chan, error: %d", err);
	}

	return err;
}

static int cmd_detect_fps(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	unsigned long fps = strtoul(argv[1], &end, 10);

	ARG_UNUSED(argc);

	if (*end != '\0' || fps < 1 || fps > CONFIG_APP_DETECTION_FPS_MAX) {
		shell_error(sh, "fps must be 1 to %d, or max", CONFIG_APP_DETECTION_FPS_MAX);
		return -EINVAL;
	}

	return publish(sh, &(struct detection_msg){ .type = DETECTION_FPS_SET, .fps = fps });
}

static int cmd_detect_fps_max(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return publish(sh, &(struct detection_msg){ .type = DETECTION_FPS_SET, .fps = 0 });
}

static int cmd_detect_confidence(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	unsigned long percent = strtoul(argv[1], &end, 10);

	ARG_UNUSED(argc);

	if (*end != '\0' || percent > 100) {
		shell_error(sh, "Confidence must be 0 to 100 (percent)");
		return -EINVAL;
	}

	return publish(sh, &(struct detection_msg){
		.type = DETECTION_THRESHOLD_SET,
		.threshold = (uint8_t)percent,
	});
}

static int cmd_detect_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return publish(sh, &(struct detection_msg){ .type = DETECTION_STATUS });
}

SHELL_STATIC_SUBCMD_SET_CREATE(detect_fps_cmds,
	SHELL_CMD(max, NULL, "Detect as fast as the decoder and the NPU keep up.",
		  cmd_detect_fps_max),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(detect_cmds,
	SHELL_CMD_ARG(fps, &detect_fps_cmds, "Set and store detections per second: <1-30>|max",
		      cmd_detect_fps, 2, 0),
	SHELL_CMD_ARG(confidence, NULL,
		      "Set and store the lowest confidence a box needs: <0-100> percent",
		      cmd_detect_confidence, 2, 0),
	SHELL_CMD(status, NULL, "Log the detection settings.", cmd_detect_status),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(detect, &detect_cmds, "Person detection", NULL);
