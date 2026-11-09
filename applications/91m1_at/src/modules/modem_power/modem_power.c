/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "modules/modem_at/modem_at.h"
#include "modem_power.h"

LOG_MODULE_REGISTER(modem_power, CONFIG_APP_MODEM_POWER_LOG_LEVEL);

#define PSM_CMD "AT+CPSMS=1,,,\"" CONFIG_APP_MODEM_POWER_PSM_TAU "\",\"" \
		CONFIG_APP_MODEM_POWER_PSM_ACTIVE_TIME "\""

static const char *const cmds[] = {
	PSM_CMD,
	/* Sleep like PSM when the network does not grant it. The device must always initiate
	 * the data connection, as the network cannot reach it while it sleeps.
	 */
	"AT%FEACONF=0,0,1",
};

void modem_power_saving_configure(void)
{
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(cmds); i++) {
		err = modem_at_run(cmds[i], NULL, 0, CONFIG_APP_MODEM_POWER_CMD_TIMEOUT_SECONDS);
		if (err) {
			LOG_WRN("%s, error: %d", cmds[i], err);
		}
	}
}
