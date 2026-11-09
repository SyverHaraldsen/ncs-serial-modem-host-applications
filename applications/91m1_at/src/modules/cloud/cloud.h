/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef CLOUD_H_
#define CLOUD_H_

#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Message types on @ref cloud_chan. */
enum cloud_msg_type {
	/**
	 * Output: the Serial Modem nRF Cloud client is no longer usable, after a
	 * @ref CLOUD_DISCONNECT.
	 */
	CLOUD_DISCONNECTED = 0x1,
	/** Output: the Serial Modem nRF Cloud client is connected. */
	CLOUD_CONNECTED,
	/**
	 * Input: the network is up, connect to nRF Cloud.
	 *
	 * The first time, the module onboards the Serial Modem with `AT#XNRFPROV`, which can
	 * take LTE down and up again. It then connects the modem's nRF Cloud client with
	 * `AT#XNRFCLOUD` and publishes @ref CLOUD_CONNECTED. A failed attempt is retried every
	 * CONFIG_APP_CLOUD_RETRY_SECONDS until @ref CLOUD_DISCONNECT. A request while
	 * connecting or connected is ignored.
	 */
	CLOUD_CONNECT,
	/**
	 * Input: the network is down, stop using the nRF Cloud connection.
	 *
	 * Publishes @ref CLOUD_DISCONNECTED when the module was connected. The Serial Modem
	 * keeps its nRF Cloud session, which the next @ref CLOUD_CONNECT reuses if it is still
	 * up. Ignored while provisioning, which takes LTE down on its own.
	 */
	CLOUD_DISCONNECT,
};

/** @brief Message published on @ref cloud_chan. */
struct cloud_msg {
	enum cloud_msg_type type;
};

/**
 * @brief Cloud module channel.
 *
 * Carries @ref CLOUD_CONNECT and @ref CLOUD_DISCONNECT to the module, and
 * @ref CLOUD_CONNECTED and @ref CLOUD_DISCONNECTED back to its observers. The module also
 * receives its own status messages and ignores them.
 */
ZBUS_CHAN_DECLARE(cloud_chan);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_H_ */
