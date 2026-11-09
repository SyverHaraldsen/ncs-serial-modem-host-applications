/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef MODEM_POWER_H_
#define MODEM_POWER_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply the modem power saving settings.
 *
 * Requests PSM with `AT+CPSMS` and enables proprietary PSM with `AT%FEACONF` on the
 * Serial Modem. Failures are logged and do not stop the remaining commands.
 *
 * Call it each time the network connects. Each command can block for up to
 * CONFIG_APP_MODEM_AT_WAIT_TIMEOUT_SECONDS plus CONFIG_APP_MODEM_POWER_CMD_TIMEOUT_SECONDS.
 */
void modem_power_saving_configure(void);

#ifdef __cplusplus
}
#endif

#endif /* MODEM_POWER_H_ */
