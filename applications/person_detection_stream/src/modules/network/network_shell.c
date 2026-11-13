/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "network.h"

static int network_publish(const struct shell *sh, enum network_msg_type type,
			   enum network_iface iface)
{
	struct network_msg msg = { .type = type, .iface = iface };
	int err;

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		shell_error(sh, "Failed to publish network message, error: %d", err);
		return err;
	}

	return 0;
}

static int cmd_network_connect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err = network_publish(sh, NETWORK_CONNECT, NETWORK_IFACE_NONE);

	if (!err) {
		shell_print(sh, "Network connect requested");
	}

	return err;
}

static int cmd_network_disconnect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err = network_publish(sh, NETWORK_DISCONNECT, NETWORK_IFACE_NONE);

	if (!err) {
		shell_print(sh, "Network disconnect requested");
	}

	return err;
}

static int use(const struct shell *sh, enum network_iface iface)
{
	return network_publish(sh, NETWORK_USE, iface);
}

static int cmd_use_auto(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return use(sh, NETWORK_IFACE_NONE);
}

static int cmd_use_wifi(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return use(sh, NETWORK_IFACE_WIFI);
}

static int cmd_use_lte(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return use(sh, NETWORK_IFACE_LTE);
}

static int cmd_network_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return network_publish(sh, NETWORK_STATUS, NETWORK_IFACE_NONE);
}

SHELL_STATIC_SUBCMD_SET_CREATE(use_cmds,
	SHELL_CMD(auto, NULL, "Wi-Fi when it is up, LTE otherwise.", cmd_use_auto),
	SHELL_CMD(wifi, NULL, "Stream over Wi-Fi once it is up, then take LTE down.",
		  cmd_use_wifi),
	SHELL_CMD(lte, NULL, "Stream over LTE once it is up, then take Wi-Fi down.",
		  cmd_use_lte),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(network_cmds,
	SHELL_CMD(connect, NULL, "Bring every interface up.", cmd_network_connect),
	SHELL_CMD(disconnect, NULL, "Take every interface down.", cmd_network_disconnect),
	SHELL_CMD(use, &use_cmds, "Choose the interface the stream uses.", NULL),
	SHELL_CMD(status, NULL, "Show interfaces and the one in use.", cmd_network_status),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(network, &network_cmds, "Network interfaces", NULL);
