/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "frame_pool.h"

K_MEM_SLAB_DEFINE_STATIC(chunk_slab, FRAME_CHUNK_SIZE, CONFIG_APP_FRAME_POOL_CHUNK_COUNT, 4);
K_MSGQ_DEFINE(chunk_q, sizeof(struct frame_chunk), CONFIG_APP_FRAME_POOL_CHUNK_COUNT, 4);

int frame_pool_get(struct frame_chunk *chunk, k_timeout_t timeout)
{
	return k_msgq_get(&chunk_q, chunk, timeout) == 0 ? 0 : -EAGAIN;
}

void frame_pool_release(struct frame_chunk *chunk)
{
	k_mem_slab_free(&chunk_slab, chunk->buf);
	chunk->buf = NULL;
}

int frame_pool_alloc(struct frame_chunk *chunk, k_timeout_t timeout)
{
	void *block;

	if (k_mem_slab_alloc(&chunk_slab, &block, timeout) != 0) {
		return -ENOMEM;
	}

	chunk->buf = block;

	return 0;
}

void frame_pool_submit(const struct frame_chunk *chunk)
{
	/* The queue has a slot for every slab block, so this never blocks. */
	(void)k_msgq_put(&chunk_q, chunk, K_FOREVER);
}

void frame_writer_init(struct frame_writer *w, uint16_t frame_id, enum frame_fmt fmt,
		       uint16_t width, uint16_t height, k_timeout_t timeout)
{
	*w = (struct frame_writer){
		.chunk = {
			.frame_id = frame_id,
			.fmt = fmt,
			.width = width,
			.height = height,
		},
		.timeout = timeout,
	};
}

static int chunk_open(struct frame_writer *w)
{
	if (frame_pool_alloc(&w->chunk, w->timeout) != 0) {
		w->failed = true;
		return -ENOMEM;
	}

	w->chunk.offset = w->total;
	w->chunk.len = 0;

	return 0;
}

void frame_writer_put(struct frame_writer *w, const void *data, size_t len)
{
	const uint8_t *src = data;

	while (len > 0 && !w->failed) {
		size_t n;

		if (w->chunk.buf == NULL && chunk_open(w) != 0) {
			return;
		}

		n = MIN(len, FRAME_CHUNK_PAYLOAD_MAX - w->chunk.len);
		memcpy(&w->chunk.buf[FRAME_CHUNK_HEADROOM + w->chunk.len], src, n);
		w->chunk.len += n;
		w->total += n;
		src += n;
		len -= n;

		if (w->chunk.len == FRAME_CHUNK_PAYLOAD_MAX) {
			frame_pool_submit(&w->chunk);
			w->chunk.buf = NULL;
		}
	}
}

int frame_writer_finish(struct frame_writer *w)
{
	if (!w->failed && w->chunk.buf == NULL) {
		(void)chunk_open(w);
	}

	if (w->failed) {
		if (w->chunk.buf != NULL) {
			frame_pool_release(&w->chunk);
		}
		return -ENOMEM;
	}

	w->chunk.flags = FRAME_CHUNK_LAST;
	w->chunk.frame_size = w->total;
	frame_pool_submit(&w->chunk);
	w->chunk.buf = NULL;

	return 0;
}

int frame_pool_write(uint16_t frame_id, enum frame_fmt fmt, uint16_t width, uint16_t height,
		     const void *data, size_t len, k_timeout_t timeout)
{
	struct frame_writer w;

	frame_writer_init(&w, frame_id, fmt, width, height, timeout);
	frame_writer_put(&w, data, len);

	return frame_writer_finish(&w);
}
