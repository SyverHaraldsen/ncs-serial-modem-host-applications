/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef DETECTION_H_
#define DETECTION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Channel carrying @ref detection_msg requests to and results from the module. */
ZBUS_CHAN_DECLARE(detection_chan);

/** @brief Message types on @ref detection_chan. */
enum detection_msg_type {
	/* To the detection module. */

	/** Set and store the detection rate in @ref detection_msg.fps. */
	DETECTION_FPS_SET = 0x1,

	/** Set and store the confidence threshold in @ref detection_msg.threshold. */
	DETECTION_THRESHOLD_SET,

	/** Stream the model input of each detected frame, see @ref detection_msg.ai_input. */
	DETECTION_AI_INPUT_SET,

	/** Log the current status. */
	DETECTION_STATUS,

	/* From the detection module. */

	/** A frame was run through the detector, see @ref detection_msg.result. */
	DETECTION_RESULT,
};

/** @brief Message carried on @ref detection_chan. */
struct detection_msg {
	enum detection_msg_type type;

	union {
		/** DETECTION_FPS_SET: detections per second, 0 for as fast as it keeps up. */
		uint8_t fps;

		/** DETECTION_THRESHOLD_SET: lowest box score kept, in percent. */
		uint8_t threshold;

		/** DETECTION_AI_INPUT_SET: true streams the model input along with the boxes. */
		bool ai_input;

		/** DETECTION_RESULT. */
		struct {
			uint16_t frame_id;
			uint8_t persons;
		} result;
	};
};

/**
 * @brief Offer a camera JPEG frame for detection.
 *
 * Copies the frame when the module is idle and a detection is due, otherwise ignores it.
 * @p width and @p height may change from frame to frame; the descale follows them.
 * Never blocks, so the producer can offer every frame it streams.
 *
 * @retval true The frame was taken.
 */
bool detection_frame_offer(const uint8_t *jpeg, size_t len, uint16_t frame_id, uint16_t width,
			   uint16_t height);

#ifdef __cplusplus
}
#endif

#endif /* DETECTION_H_ */
