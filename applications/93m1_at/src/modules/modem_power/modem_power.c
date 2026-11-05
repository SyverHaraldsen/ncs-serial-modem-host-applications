/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/device.h>
#include <zephyr/drivers/modem/modem_cellular.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "modules/modem_at/modem_at.h"
#include "modem_power.h"

LOG_MODULE_REGISTER(modem_power, CONFIG_APP_MODEM_POWER_LOG_LEVEL);

#define PSM_CMD "AT+CPSMS=1,,,\"" CONFIG_APP_MODEM_POWER_PSM_TAU "\",\"" \
		CONFIG_APP_MODEM_POWER_PSM_ACTIVE_TIME "\""

static const char *const cmds[] = {
	PSM_CMD,
	"AT%MODULECFG=\"ledMode\",0",
	"AT%MODULECFG=\"logCtrl\",0",
	"AT%MODULECFG=\"usbCtrl\",2",
	"AT%MODULECFG=\"usbSlpMask\",1",
	"AT%SIMCFG=\"SimPowerSave\",1",
	"AT%SIMCFG=\"SimPresenceDetect\",0",
	/* autoPowerSave is left off. The modem then sleeps behind the host's back, and a host
	 * reset leaves it unreachable, as the host cannot wake it without CMUX power save.
	 */
};

void modem_power_saving_configure(void)
{
	/* The driver's periodic poll would wake the link every 30 s, and its response is
	 * never read, since the location module queries the modem on demand.
	 */
	int err = cellular_modem_pause_periodic_script(DEVICE_DT_GET(DT_ALIAS(modem)));

	if (err && err != -EINVAL && err != -ENOTSUP) {
		LOG_WRN("cellular_modem_pause_periodic_script, error: %d", err);
	}

	for (size_t i = 0; i < ARRAY_SIZE(cmds); i++) {
		err = modem_at_run(cmds[i], NULL, 0, CONFIG_APP_MODEM_POWER_CMD_TIMEOUT_SECONDS);
		if (err) {
			LOG_WRN("%s, error: %d", cmds[i], err);
		}
	}
}
