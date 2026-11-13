/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(network, CONFIG_APP_NETWORK_LOG_LEVEL);

#if CONFIG_APP_NETWORK_MODEM_BAUDRATE > 0

#define MODEM_BAUDRATE_INIT_PRIORITY 60

BUILD_ASSERT(MODEM_BAUDRATE_INIT_PRIORITY > CONFIG_SERIAL_INIT_PRIORITY &&
	     MODEM_BAUDRATE_INIT_PRIORITY < CONFIG_MODEM_CELLULAR_INIT_PRIORITY);

static int modem_baudrate_set(void)
{
	const struct device *uart = DEVICE_DT_GET(DT_BUS(DT_ALIAS(modem)));
	struct uart_config cfg;
	int err;

	err = uart_config_get(uart, &cfg);
	if (!err) {
		cfg.baudrate = CONFIG_APP_NETWORK_MODEM_BAUDRATE;
		err = uart_configure(uart, &cfg);
	}

	if (err) {
		LOG_ERR("Modem UART baud rate %d, error: %d", CONFIG_APP_NETWORK_MODEM_BAUDRATE,
			err);
	}

	return 0;
}

SYS_INIT(modem_baudrate_set, POST_KERNEL, MODEM_BAUDRATE_INIT_PRIORITY);

#endif
