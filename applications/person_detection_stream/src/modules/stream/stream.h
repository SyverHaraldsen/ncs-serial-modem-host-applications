/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef STREAM_H_
#define STREAM_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Channel carrying @ref stream_msg requests to the stream module. */
ZBUS_CHAN_DECLARE(stream_chan);

#define STREAM_HOST_MAX_LEN 63

/** @brief Message types handled by the stream module. */
enum stream_msg_type {
	/* From main: the network is up or down. */

	/**
	 * Open the socket over @ref stream_msg.iface, provided streaming is started and an
	 * endpoint is set. Sent again with another interface, the socket moves to it.
	 */
	STREAM_CONNECT = 0x1,

	/** Close the socket. */
	STREAM_DISCONNECT,

	/* From the user. */

	/** Stream whenever connected. The default at boot. */
	STREAM_START,

	/** Stop streaming until STREAM_START or the next boot. */
	STREAM_STOP,

	/** Set and store the endpoint in @ref stream_msg.host and @ref stream_msg.port. */
	STREAM_ENDPOINT,

	/** Log the current status. */
	STREAM_STATUS,
};

/** @brief Message carried on @ref stream_chan. */
struct stream_msg {
	enum stream_msg_type type;

	/** STREAM_CONNECT only: net_if index of the interface to stream over. */
	int iface;

	/** STREAM_ENDPOINT only. */
	char host[STREAM_HOST_MAX_LEN + 1];
	uint16_t port;
};

#ifdef __cplusplus
}
#endif

#endif /* STREAM_H_ */
