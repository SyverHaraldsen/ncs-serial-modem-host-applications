/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "jpeg.h"
#include "JPEGDEC.h"
#include "jpeg.inl"

LOG_MODULE_REGISTER(jpeg, LOG_LEVEL_INF);

struct decode_ctx {
	jpeg_block_sink_t sink;
	void *ctx;
};

/* About 17.6 KB: Huffman tables, MCU and pixel buffers. */
static JPEGIMAGE image;

static const int scale_options[] = {
	0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH,
};

static int draw(JPEGDRAW *block)
{
	const struct decode_ctx *dec = block->pUser;

	dec->sink(dec->ctx, block->x, block->y, block->iWidthUsed, block->iHeight, block->iWidth,
		  block->pPixels);

	return 1;
}

int jpeg_decode(const uint8_t *data, size_t len, uint8_t scale_shift, jpeg_block_sink_t sink,
		void *ctx)
{
	struct decode_ctx dec = { .sink = sink, .ctx = ctx };
	int err;
	int ok;

	if (scale_shift >= ARRAY_SIZE(scale_options)) {
		return -EINVAL;
	}

	if (!JPEG_openRAM(&image, (uint8_t *)data, (int)len, draw)) {
		LOG_WRN("JPEG_openRAM, error: %d", JPEG_getLastError(&image));
		return -EINVAL;
	}

	JPEG_setPixelType(&image, RGB565_LITTLE_ENDIAN);
	image.pUser = &dec;

	ok = JPEG_decode(&image, 0, 0, scale_options[scale_shift]);
	err = JPEG_getLastError(&image);
	JPEG_close(&image);

	if (!ok) {
		LOG_WRN("JPEG_decode, error: %d", err);
		return -EIO;
	}

	return 0;
}
