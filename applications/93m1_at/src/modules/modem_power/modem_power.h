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
 * Pauses the modem driver's periodic poll, which would otherwise wake the CMUX link,
 * and sends PSM and nRF93M1 module power saving commands over the AT pipe. Failures
 * are logged and do not stop the remaining commands.
 *
 * Call it each time the network connects, as the modem does not retain all settings
 * across a reset.
 */
void modem_power_saving_configure(void);

#ifdef __cplusplus
}
#endif

#endif /* MODEM_POWER_H_ */
