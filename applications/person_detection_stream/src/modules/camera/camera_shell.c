/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "camera.h"

static int publish(const struct shell *sh, const struct camera_msg *msg)
{
	int err = zbus_chan_pub(&camera_chan, msg, PUB_TIMEOUT);

	if (err) {
		shell_error(sh, "zbus_chan_pub camera_chan, error: %d", err);
	}

	return err;
}

static int cmd_camera_fps(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	unsigned long fps = strtoul(argv[1], &end, 10);

	ARG_UNUSED(argc);

	if (*end != '\0' || fps < 1 || fps > CONFIG_APP_CAMERA_FPS_MAX) {
		shell_error(sh, "fps must be 1 to %d, or max", CONFIG_APP_CAMERA_FPS_MAX);
		return -EINVAL;
	}

	return publish(sh, &(struct camera_msg){ .type = CAMERA_FPS_SET, .fps = fps });
}

static int cmd_camera_fps_max(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return publish(sh, &(struct camera_msg){ .type = CAMERA_FPS_SET, .fps = 0 });
}

static int cmd_camera_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return publish(sh, &(struct camera_msg){ .type = CAMERA_STATUS });
}

static int profile_publish(const struct shell *sh, enum camera_profile profile)
{
	return publish(sh, &(struct camera_msg){ .type = CAMERA_PROFILE_SET,
						 .profile = profile });
}

static int cmd_camera_profile_auto(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return profile_publish(sh, CAMERA_PROFILE_AUTO);
}

static int cmd_camera_profile_low(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return profile_publish(sh, CAMERA_PROFILE_LOW);
}

SHELL_STATIC_SUBCMD_SET_CREATE(camera_profile_cmds,
	SHELL_CMD(auto, NULL, "High on Wi-Fi, low on LTE.", cmd_camera_profile_auto),
	SHELL_CMD(low, NULL, "Small frames, sized for LTE-M.", cmd_camera_profile_low),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(camera_fps_cmds,
	SHELL_CMD(max, NULL, "Every frame the camera delivers.", cmd_camera_fps_max),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(camera_cmds,
	SHELL_CMD_ARG(fps, &camera_fps_cmds, "Set and store frames per second: <1-30>|max",
		      cmd_camera_fps, 2, 0),
	SHELL_CMD(profile, &camera_profile_cmds, "Set and store the frame size profile.", NULL),
	SHELL_CMD(status, NULL, "Log the camera settings.", cmd_camera_status),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(camera, &camera_cmds, "Camera control", NULL);
