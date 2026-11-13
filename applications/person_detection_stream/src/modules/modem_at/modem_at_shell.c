/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "modem_at.h"

static int cmd_at(const struct shell *sh, size_t argc, char **argv)
{
	static char resp[CONFIG_APP_MODEM_AT_RESPONSE_MAX_SIZE];
	int ret;

	if (argc != 2) {
		shell_error(sh, "usage: at \"<command>\"");
		return -EINVAL;
	}

	ret = modem_at_run(argv[1], resp, sizeof(resp), CONFIG_APP_MODEM_AT_TIMEOUT_SECONDS);

	if (resp[0] != '\0') {
		shell_print(sh, "%s", resp);
	}

	switch (ret) {
	case 0:
		shell_print(sh, "OK");
		break;
	case -EPERM:
		shell_error(sh, "modem is not ready");
		break;
	case -EBUSY:
		shell_error(sh, "AT pipe busy");
		break;
	default:
		shell_error(sh, "AT command failed (%d)", ret);
		break;
	}

	return ret;
}

SHELL_CMD_ARG_REGISTER(at, NULL, "Send AT command: at \"<command>\"", cmd_at, 2, 0);
