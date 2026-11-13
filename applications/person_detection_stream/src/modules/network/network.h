/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef NETWORK_H_
#define NETWORK_H_

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

ZBUS_CHAN_DECLARE(network_chan);

enum network_iface {
	/** No interface; as a NETWORK_USE target it selects automatic choice. */
	NETWORK_IFACE_NONE = 0,
	NETWORK_IFACE_WIFI,
	NETWORK_IFACE_LTE,
	NETWORK_IFACE_COUNT,
};

enum network_msg_type {
	/* Reported by the network module. */

	/** @ref network_msg.iface has an IPv4 address and can carry traffic. */
	NETWORK_IFACE_UP = 0x1,

	/** @ref network_msg.iface can no longer carry traffic. */
	NETWORK_IFACE_DOWN,

	/* Requested of the network module. */

	/** Bring every interface up. */
	NETWORK_CONNECT,

	/** Take every interface down. */
	NETWORK_DISCONNECT,

	/** Bring @ref network_msg.iface up. */
	NETWORK_IFACE_CONNECT,

	/** Take @ref network_msg.iface down. */
	NETWORK_IFACE_DISCONNECT,

	/* Requested of main. */

	/**
	 * Stream over @ref network_msg.iface, or choose automatically for NETWORK_IFACE_NONE.
	 * The stream stays on the current interface until the new one is up.
	 */
	NETWORK_USE,

	/** Log which interfaces are up and which one carries the stream. */
	NETWORK_STATUS,

	/* Internal to the network module. */

	/** LTE did not come up within CONFIG_APP_NETWORK_LTE_RECOVERY_SECONDS. */
	NETWORK_LTE_TIMEOUT,
};

struct network_msg {
	enum network_msg_type type;
	enum network_iface iface;
};

/** @brief The interface behind @p iface, or NULL when the board has none. */
struct net_if *network_iface_get(enum network_iface iface);

/** @brief Short name of @p iface for logs and the shell. */
const char *network_iface_name(enum network_iface iface);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_H_ */
