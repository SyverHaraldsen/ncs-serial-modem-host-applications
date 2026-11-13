/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef MODEM_AT_H_
#define MODEM_AT_H_

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run an AT command synchronously on the shared user pipe (DLCI 3).
 *
 * Blocks until OK/ERROR or timeout. Response lines are newline-joined into
 * @p resp. Serialised internally, so the modem-at shell and application
 * modules can share the single user pipe.
 *
 * @param req       AT command string (without trailing CR).
 * @param resp      Buffer for the response, or NULL to discard.
 * @param resp_size Size of @p resp.
 * @param timeout_s Seconds to wait before aborting.
 *
 * @retval 0        Modem replied OK.
 * @retval -EPERM   Pipe not ready (modem not attached yet).
 * @retval -EBUSY   Another command is already running.
 * @retval -EIO     Modem replied ERROR or timed out.
 * @retval -EINVAL  Invalid request.
 */
int modem_at_run(const char *req, char *resp, size_t resp_size, uint32_t timeout_s);

#ifdef __cplusplus
}
#endif

#endif /* MODEM_AT_H_ */
