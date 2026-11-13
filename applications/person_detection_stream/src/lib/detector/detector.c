/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include <axon/nrf_axon_platform.h>
#include <drivers/axon/nrf_axon_driver.h>
#include <drivers/axon/nrf_axon_nn_infer.h>

#include "generated/nrf_axon_model_person_det_.h"
#include "postprocessing.h"

#include "detector.h"

LOG_MODULE_REGISTER(detector, CONFIG_APP_DETECTOR_LOG_LEVEL);

#define MODEL_WIDTH DETECTOR_INPUT_WIDTH
#define MODEL_HEIGHT DETECTOR_INPUT_HEIGHT
#define MODEL_PLANE (MODEL_WIDTH * MODEL_HEIGHT)

static const nrf_axon_nn_compiled_model_s *const model = &model_person_det;

static int8_t input_buf[MODEL_PLANE * 3];
static int8_t output_buf[NRF_AXON_MODEL_PERSON_DET_PACKED_OUTPUT_SIZE];
static int8_t lut[256];
static atomic_t min_score_permille = ATOMIC_INIT(CONFIG_APP_DETECTOR_MIN_SCORE * 10);
static uint8_t unlut[256];

/* The fed image sits centred in the model input; the border stays neutral gray. */
static struct {
	uint16_t width;
	uint16_t height;
	uint16_t pad_left;
	uint16_t pad_top;
	uint8_t scale_shift;
} geometry;

static int8_t quantize(float value, const nrf_axon_nn_compiled_model_input_s *in)
{
	const float scale = (float)in->quant_mult / (float)(1 << in->quant_round);

	return (int8_t)__SSAT((int32_t)(value * scale) + in->quant_zp, 8);
}

/* Maps channel values onto the model's symmetric [-1, 1) input range. */
static void model_input_init(const nrf_axon_nn_compiled_model_input_s *in)
{
	memset(input_buf, quantize(0.0f, in), sizeof(input_buf));

	for (size_t i = 0; i < ARRAY_SIZE(lut); i++) {
		lut[i] = quantize(((float)i / ARRAY_SIZE(lut)) * 2.0f - 1.0f, in);
	}

	/* Quantized values the LUT never produces map to the nearest lower channel value. */
	for (size_t q = 0, i = 0; q < ARRAY_SIZE(unlut); q++) {
		const int8_t value = (int8_t)(q - 128);

		while (i + 1 < ARRAY_SIZE(lut) && lut[i + 1] <= value) {
			i++;
		}
		unlut[(uint8_t)value] = (uint8_t)i;
	}
}

int detector_geometry_set(uint16_t width, uint16_t height, uint8_t scale_shift)
{
	if (width > MODEL_WIDTH || height > MODEL_HEIGHT) {
		LOG_ERR("Image %ux%u does not fit the %ux%u model input", width, height,
			MODEL_WIDTH, MODEL_HEIGHT);
		return -EINVAL;
	}

	/* A smaller image leaves old pixels in what is now border. */
	memset(input_buf, lut[ARRAY_SIZE(lut) / 2], sizeof(input_buf));

	geometry.width = width;
	geometry.height = height;
	geometry.pad_left = (MODEL_WIDTH - width) / 2;
	geometry.pad_top = (MODEL_HEIGHT - height) / 2;
	geometry.scale_shift = scale_shift;

	return 0;
}

int detector_init(uint16_t width, uint16_t height, uint8_t scale_shift)
{
	const nrf_axon_nn_compiled_model_input_s *in = &model->inputs[0];
	int err;

	if (nrf_axon_platform_init() != NRF_AXON_RESULT_SUCCESS) {
		LOG_ERR("Axon platform init failed");
		return -EIO;
	}

	if (nrf_axon_nn_model_validate(model) != NRF_AXON_RESULT_SUCCESS) {
		LOG_ERR("Model %s failed validation", model->model_name);
		return -EINVAL;
	}

	if (in->dimensions.width != MODEL_WIDTH || in->dimensions.height != MODEL_HEIGHT) {
		LOG_ERR("Model input is %ux%u, expected %ux%u", in->dimensions.width,
			in->dimensions.height, MODEL_WIDTH, MODEL_HEIGHT);
		return -EINVAL;
	}

	model_input_init(in);
	decode_init(model);

	err = detector_geometry_set(width, height, scale_shift);
	if (err) {
		return err;
	}

	LOG_INF("Person detection with %s on %ux%u images", model->model_name, width, height);

	return 0;
}

void detector_feed_rgb565(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
			  uint16_t pitch, const uint16_t *pixels)
{
	for (uint16_t row = 0; row < height && y + row < geometry.height; row++) {
		const size_t dst_row = (size_t)(geometry.pad_top + y + row) * MODEL_WIDTH +
				       geometry.pad_left + x;
		const uint16_t *src = &pixels[(size_t)row * pitch];

		for (uint16_t col = 0; col < width && x + col < geometry.width; col++) {
			const uint16_t v = src[col];
			const size_t dst = dst_row + col;

			input_buf[dst] = lut[((v >> 8) & 0xF8) | (v >> 13)];
			input_buf[MODEL_PLANE + dst] = lut[((v >> 3) & 0xFC) | ((v >> 9) & 0x3)];
			input_buf[2 * MODEL_PLANE + dst] = lut[((v << 3) & 0xF8) | ((v >> 2) & 0x7)];
		}
	}
}

void detector_input_row_rgb565(uint16_t row, uint8_t *out)
{
	const size_t base = (size_t)row * MODEL_WIDTH;

	for (size_t col = 0; col < MODEL_WIDTH; col++) {
		const uint8_t r = unlut[(uint8_t)input_buf[base + col]];
		const uint8_t g = unlut[(uint8_t)input_buf[MODEL_PLANE + base + col]];
		const uint8_t b = unlut[(uint8_t)input_buf[2 * MODEL_PLANE + base + col]];

		sys_put_be16(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3), &out[2 * col]);
	}
}

void detector_min_score_set(uint8_t percent)
{
	atomic_set(&min_score_permille, MIN(percent, 100) * 10);
}

static int16_t to_camera(float model_coord, int pad, int limit)
{
	return (int16_t)(CLAMP((int)model_coord - pad, 0, limit) << geometry.scale_shift);
}

int detector_run(struct detector_box *boxes, size_t max)
{
	struct detection_box found[CONFIG_APP_DETECTOR_MAX_BOXES];
	size_t count;

	if (nrf_axon_nn_model_infer_sync(model, input_buf, output_buf) != NRF_AXON_RESULT_SUCCESS) {
		LOG_ERR("Inference failed");
		return -EIO;
	}

	const size_t found_count = decode_output(model, output_buf, found,
						 MIN(max, ARRAY_SIZE(found)));
	const uint16_t min_score = (uint16_t)atomic_get(&min_score_permille);

	count = 0;
	for (size_t i = 0; i < found_count; i++) {
		const uint16_t score = (uint16_t)(found[i].score * 1000.0f);

		if (score < min_score) {
			continue;
		}

		boxes[count++] = (struct detector_box){
			.x1 = to_camera(found[i].x1, geometry.pad_left, geometry.width),
			.y1 = to_camera(found[i].y1, geometry.pad_top, geometry.height),
			.x2 = to_camera(found[i].x2, geometry.pad_left, geometry.width),
			.y2 = to_camera(found[i].y2, geometry.pad_top, geometry.height),
			.score = score,
		};
	}

	return (int)count;
}
