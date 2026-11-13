/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef JPEG_H_
#define JPEG_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Receives one decoded block of RGB565 pixels in native byte order, placed at
 * @p x, @p y in the descaled image. Row r of the block starts at
 * pixels[r * pitch]. Called many times per frame, top to bottom.
 */
typedef void (*jpeg_block_sink_t)(void *ctx, uint16_t x, uint16_t y, uint16_t width,
				  uint16_t height, uint16_t pitch, const uint16_t *pixels);

/**
 * @brief Decode a baseline JPEG, descaled on the fly.
 *
 * @param data        Complete JPEG stream.
 * @param len         Length of @p data.
 * @param scale_shift Output is 1/2^scale_shift of the image size, 0 to 3.
 * @param sink        Output callback.
 * @param ctx         Passed to @p sink.
 *
 * @retval 0       Frame decoded.
 * @retval -EINVAL Not a JPEG the decoder supports.
 * @retval -EIO    Corrupt stream.
 */
int jpeg_decode(const uint8_t *data, size_t len, uint8_t scale_shift, jpeg_block_sink_t sink,
		void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_H_ */
