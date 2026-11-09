/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "buttons.h"

static int publish_button(const struct shell *sh, enum button_msg_type type)
{
	const struct button_msg msg = { .type = type };
	int err;

	err = zbus_chan_pub(&button_chan, &msg, PUB_TIMEOUT);
	if (err) {
		shell_error(sh, "zbus_chan_pub button_chan, error: %d", err);
	}

	return err;
}

static int cmd_button_1(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return publish_button(sh, BUTTON_1);
}

static int cmd_button_2(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return publish_button(sh, BUTTON_2);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_cmds,
	SHELL_CMD(1, NULL, "Simulate a button 1 press.", cmd_button_1),
	SHELL_CMD(2, NULL, "Simulate a button 2 press.", cmd_button_2),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(button, &sub_cmds, "Simulate a physical button press", NULL);
