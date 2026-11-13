/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef DETECTOR_H_
#define DETECTOR_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A detected person in pixels of the camera frame. */
struct detector_box {
	int16_t x1;
	int16_t y1;
	int16_t x2;
	int16_t y2;
	/** Confidence in per mille. */
	uint16_t score;
};

/**
 * @brief Bring up the Axon NPU and the person_det model.
 *
 * @param width       Width of the images that will be fed, at most the model input width.
 * @param height      Height of the images that will be fed, at most the model input height.
 * @param scale_shift Boxes are scaled up by 2^scale_shift to camera frame pixels.
 */
int detector_init(uint16_t width, uint16_t height, uint8_t scale_shift);

/**
 * @brief Change the size of the images fed from now on, see detector_init().
 */
int detector_geometry_set(uint16_t width, uint16_t height, uint8_t scale_shift);

/**
 * @brief Convert a block of RGB565 pixels into the model input.
 *
 * @param x      Left edge of the block in the fed image.
 * @param y      Top edge of the block in the fed image.
 * @param width  Block width.
 * @param height Block height.
 * @param pitch  Pixels from one block row to the next.
 * @param pixels Block pixels in native byte order.
 */
void detector_feed_rgb565(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
			  uint16_t pitch, const uint16_t *pixels);

/** Width and height of the model input, see detector_input_row_rgb565(). */
#define DETECTOR_INPUT_WIDTH 160
#define DETECTOR_INPUT_HEIGHT 128

/**
 * @brief Read one row of the model input back as RGB565 big-endian, for viewing.
 *
 * @param row Row index, below DETECTOR_INPUT_HEIGHT.
 * @param out DETECTOR_INPUT_WIDTH * 2 bytes.
 */
void detector_input_row_rgb565(uint16_t row, uint8_t *out);

/**
 * @brief Drop boxes scoring below @p percent. Values below CONFIG_SCORE_THRESHOLD act
 *        as that floor.
 */
void detector_min_score_set(uint8_t percent);

/**
 * @brief Run the model on the frame fed so far.
 *
 * @param boxes Output boxes, strongest first.
 * @param max   Capacity of @p boxes.
 *
 * @return Number of boxes written, or a negative error code.
 */
int detector_run(struct detector_box *boxes, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* DETECTOR_H_ */
