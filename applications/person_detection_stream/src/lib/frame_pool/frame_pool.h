/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef FRAME_POOL_H_
#define FRAME_POOL_H_

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes reserved at the start of every chunk buffer for the stream header. */
#define FRAME_CHUNK_HEADROOM 28

/** Size of one chunk buffer, header room included. Each chunk becomes one datagram. */
#define FRAME_CHUNK_SIZE CONFIG_APP_STREAM_DATAGRAM_SIZE

/** Maximum payload carried by one chunk. */
#define FRAME_CHUNK_PAYLOAD_MAX (FRAME_CHUNK_SIZE - FRAME_CHUNK_HEADROOM)

enum frame_fmt {
	FRAME_FMT_NONE = 0,
	/* 2 was GRAY8; kept free so receivers stay compatible. */
	/** 16 bits per pixel, high byte first. */
	FRAME_FMT_RGB565_BE = 1,
	FRAME_FMT_JPEG = 3,
	/** Detection boxes for the image frame with the same frame_id, see frame_detection. */
	FRAME_FMT_DETECTIONS = 4,
};

/** Set on the final chunk of a frame. */
#define FRAME_CHUNK_LAST BIT(0)

/**
 * A FRAME_FMT_DETECTIONS payload is a count byte and a reserved byte followed by one
 * record per box: x1, y1, x2, y2 as big-endian int16 pixels of the image frame, then
 * the score as a big-endian uint16 in per mille.
 */
#define FRAME_DETECTION_HEADER_SIZE 2
#define FRAME_DETECTION_SIZE 10

/** A slice of a frame, sized to fit in one datagram. */
struct frame_chunk {
	/** Chunk buffer of FRAME_CHUNK_SIZE bytes; payload starts at FRAME_CHUNK_HEADROOM. */
	uint8_t *buf;
	/** Payload length in bytes. */
	uint16_t len;
	uint16_t frame_id;
	uint8_t fmt;
	/** FRAME_CHUNK_* flags. */
	uint8_t flags;
	uint16_t width;
	uint16_t height;
	/** Total frame size in bytes; 0 until the last chunk if the source does not know it. */
	uint32_t frame_size;
	/** Offset of this chunk's payload within the frame. */
	uint32_t offset;
};

/**
 * @brief Wait for the next frame chunk.
 *
 * @retval 0        @p chunk filled in; return it with frame_pool_release().
 * @retval -EAGAIN  No chunk within @p timeout.
 */
int frame_pool_get(struct frame_chunk *chunk, k_timeout_t timeout);

/** @brief Hand a chunk obtained from frame_pool_get() back to the pool. */
void frame_pool_release(struct frame_chunk *chunk);

/**
 * @brief Producer: take an empty chunk buffer from the pool into @p chunk->buf.
 *
 * Blocks while every buffer is queued or held by the consumer, which paces the source.
 *
 * @retval 0        @p chunk->buf points at a FRAME_CHUNK_SIZE buffer.
 * @retval -ENOMEM  No buffer within @p timeout.
 */
int frame_pool_alloc(struct frame_chunk *chunk, k_timeout_t timeout);

/** @brief Producer: queue a filled chunk for frame_pool_get(). */
void frame_pool_submit(const struct frame_chunk *chunk);

/** Slices one frame into chunks; the last one carries FRAME_CHUNK_LAST and the frame size. */
struct frame_writer {
	struct frame_chunk chunk;
	k_timeout_t timeout;
	uint32_t total;
	bool failed;
};

/**
 * @brief Start a frame.
 *
 * @param timeout How long each chunk allocation may wait for a free buffer.
 */
void frame_writer_init(struct frame_writer *w, uint16_t frame_id, enum frame_fmt fmt,
		       uint16_t width, uint16_t height, k_timeout_t timeout);

/** @brief Append @p len bytes, submitting every chunk that fills up. */
void frame_writer_put(struct frame_writer *w, const void *data, size_t len);

/**
 * @brief Submit the last chunk.
 *
 * @retval 0       Whole frame submitted.
 * @retval -ENOMEM The pool ran dry; the chunks already submitted form an incomplete frame
 *                 the receiver drops.
 */
int frame_writer_finish(struct frame_writer *w);

/** @brief Submit a whole frame held in one buffer, see frame_writer_finish(). */
int frame_pool_write(uint16_t frame_id, enum frame_fmt fmt, uint16_t width, uint16_t height,
		     const void *data, size_t len, k_timeout_t timeout);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_POOL_H_ */
