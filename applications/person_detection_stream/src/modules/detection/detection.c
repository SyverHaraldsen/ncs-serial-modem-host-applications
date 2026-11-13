/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/smf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "detection.h"
#include "lib/detector/detector.h"
#include "lib/frame_pool/frame_pool.h"
#include "lib/jpeg/jpeg.h"

LOG_MODULE_REGISTER(detection, CONFIG_APP_DETECTION_LOG_LEVEL);

/* Below the camera and stream threads, so detection only gets the CPU time they leave. */
#define THREAD_PRIORITY 11
#define WAIT K_MSEC(MSEC_PER_SEC)
#define STATS_INTERVAL_MS (CONFIG_APP_DETECTION_STATS_INTERVAL_SECONDS * MSEC_PER_SEC)
#define MAX_SCALE_SHIFT 3
BUILD_ASSERT(FRAME_DETECTION_HEADER_SIZE +
	     CONFIG_APP_DETECTOR_MAX_BOXES * FRAME_DETECTION_SIZE <= FRAME_CHUNK_PAYLOAD_MAX);

/* Published by detection_frame_offer() once the slot holds a new frame. */
struct detection_frame_msg {
	uint16_t frame_id;
	uint16_t width;
	uint16_t height;
	uint32_t len;
};

ZBUS_CHAN_DEFINE(detection_chan,
		 struct detection_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(detection_frame_chan,
		 struct detection_frame_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(detection);

#define CHANNEL_LIST(X) \
	X(detection_chan, struct detection_msg) \
	X(detection_frame_chan, struct detection_frame_msg)

#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, detection, 0);

CHANNEL_LIST(ADD_OBSERVERS)

/*
 * The slot belongs to detection_frame_offer() while accepting is set, which only
 * STATE_IDLE's entry does, and to the detection thread from the offer that clears it.
 */
static uint8_t slot[CONFIG_APP_DETECTION_SLOT_SIZE] __aligned(4);
static atomic_t accepting;
static atomic_t due_ms;
static atomic_t too_large;

enum detection_state {
	/** The detector is up. Handles configuration for its children. */
	STATE_ACTIVE,
	/** Waiting for a frame in the slot. */
	STATE_IDLE,
	/** Decoding and detecting the frame in the slot. */
	STATE_DETECTING,
	/** The detector failed to start; frames are never taken. */
	STATE_DISABLED,
};

struct detection_stats {
	int64_t window_start_ms;
	uint32_t detected;
	uint32_t failed;
	uint32_t decode_ms;
	uint32_t infer_ms;
	uint32_t send_ms;
	int last_persons;
};

struct detection_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	uint8_t fps;
	uint8_t threshold;
	bool ai_input;
	/* Size of the frames the detector is set up for. */
	uint16_t width;
	uint16_t height;
	uint8_t scale_shift;
	struct detection_stats stats;
};

static enum smf_state_result active_run(void *obj);
static void idle_entry(void *obj);
static enum smf_state_result idle_run(void *obj);
static void detecting_entry(void *obj);
static enum smf_state_result disabled_run(void *obj);

static const struct smf_state states[] = {
	[STATE_ACTIVE] = SMF_CREATE_STATE(NULL, active_run, NULL, NULL, &states[STATE_IDLE]),
	[STATE_IDLE] = SMF_CREATE_STATE(idle_entry, idle_run, NULL, &states[STATE_ACTIVE], NULL),
	[STATE_DETECTING] = SMF_CREATE_STATE(detecting_entry, NULL, NULL, &states[STATE_ACTIVE],
					     NULL),
	[STATE_DISABLED] = SMF_CREATE_STATE(NULL, disabled_run, NULL, NULL, NULL),
};

static struct detection_state_object detection_state = {
	.fps = CONFIG_APP_DETECTION_FPS,
	.threshold = CONFIG_APP_DETECTOR_MIN_SCORE,
};

static int settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t *target;
	uint8_t max;
	uint8_t value;
	ssize_t n;

	if (strcmp(key, "fps") == 0) {
		target = &detection_state.fps;
		max = CONFIG_APP_DETECTION_FPS_MAX;
	} else if (strcmp(key, "threshold") == 0) {
		target = &detection_state.threshold;
		max = 100;
	} else {
		return -ENOENT;
	}

	if (len != sizeof(value)) {
		return -EINVAL;
	}

	n = read_cb(cb_arg, &value, len);
	if (n < 0) {
		return n;
	}

	*target = MIN(value, max);

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(detection, "detection", NULL, settings_set, NULL, NULL);

static void setting_store(const char *key, uint8_t value)
{
	int err = settings_save_one(key, &value, sizeof(value));

	if (err) {
		LOG_ERR("settings_save_one %s, error: %d", key, err);
	}
}

bool detection_frame_offer(const uint8_t *jpeg, size_t len, uint16_t frame_id, uint16_t width,
			   uint16_t height)
{
	const struct detection_frame_msg msg = {
		.frame_id = frame_id, .width = width, .height = height, .len = len,
	};

	if ((int32_t)(k_uptime_get_32() - (uint32_t)atomic_get(&due_ms)) < 0) {
		return false;
	}

	if (len > sizeof(slot)) {
		atomic_inc(&too_large);
		return false;
	}

	if (!atomic_cas(&accepting, true, false)) {
		return false;
	}

	memcpy(slot, jpeg, len);

	if (zbus_chan_pub(&detection_frame_chan, &msg, K_NO_WAIT) != 0) {
		atomic_set(&accepting, true);
		return false;
	}

	return true;
}

static void status_log(const struct detection_state_object *state_object)
{
	char fps[4] = "max";

	if (state_object->fps) {
		snprintk(fps, sizeof(fps), "%u", state_object->fps);
	}

	LOG_INF("Detection fps %s, confidence %u%% (at least %d%%), ai-input stream %s", fps,
		state_object->threshold, CONFIG_SCORE_THRESHOLD / 10,
		state_object->ai_input ? "on" : "off");
}

static void result_publish(uint16_t frame_id, int persons)
{
	const struct detection_msg msg = {
		.type = DETECTION_RESULT,
		.result = { .frame_id = frame_id, .persons = (uint8_t)persons },
	};
	int err = zbus_chan_pub(&detection_chan, &msg, PUB_TIMEOUT);

	if (err) {
		LOG_ERR("zbus_chan_pub detection_chan, error: %d", err);
		FATAL_ERROR();
	}
}

static void detector_sink(void *ctx, uint16_t x, uint16_t y, uint16_t width, uint16_t height,
			  uint16_t pitch, const uint16_t *pixels)
{
	ARG_UNUSED(ctx);

	detector_feed_rgb565(x, y, width, height, pitch, pixels);
}

/* Boxes drawn on the model input view need its coordinates instead of the camera's. */
static void boxes_to_model_input(const struct detection_state_object *state_object,
				 struct detector_box *boxes, int count)
{
	const uint8_t shift = state_object->scale_shift;
	const int16_t pad_left = (DETECTOR_INPUT_WIDTH - (state_object->width >> shift)) / 2;
	const int16_t pad_top = (DETECTOR_INPUT_HEIGHT - (state_object->height >> shift)) / 2;

	for (int i = 0; i < count; i++) {
		boxes[i].x1 = (boxes[i].x1 >> shift) + pad_left;
		boxes[i].y1 = (boxes[i].y1 >> shift) + pad_top;
		boxes[i].x2 = (boxes[i].x2 >> shift) + pad_left;
		boxes[i].y2 = (boxes[i].y2 >> shift) + pad_top;
	}
}

/* Descales the frame by the smallest power of two that fits the model input. */
static int geometry_follow(struct detection_state_object *state_object, uint16_t width,
			   uint16_t height)
{
	uint8_t shift = 0;
	int err;

	if (width == state_object->width && height == state_object->height) {
		return 0;
	}

	while ((width >> shift) > DETECTOR_INPUT_WIDTH ||
	       (height >> shift) > DETECTOR_INPUT_HEIGHT) {
		if (++shift > MAX_SCALE_SHIFT) {
			return -EINVAL;
		}
	}

	err = detector_geometry_set(width >> shift, height >> shift, shift);
	if (err) {
		return err;
	}

	LOG_INF("Detecting on %ux%u frames at 1/%u scale", width, height, 1U << shift);

	state_object->width = width;
	state_object->height = height;
	state_object->scale_shift = shift;

	return 0;
}

static void detections_send(uint16_t frame_id, const struct detector_box *boxes, int count,
			    uint16_t width, uint16_t height)
{
	uint8_t payload[FRAME_DETECTION_HEADER_SIZE +
			CONFIG_APP_DETECTOR_MAX_BOXES * FRAME_DETECTION_SIZE] = { (uint8_t)count };
	uint8_t *p = &payload[FRAME_DETECTION_HEADER_SIZE];

	for (int i = 0; i < count; i++, p += FRAME_DETECTION_SIZE) {
		sys_put_be16(boxes[i].x1, &p[0]);
		sys_put_be16(boxes[i].y1, &p[2]);
		sys_put_be16(boxes[i].x2, &p[4]);
		sys_put_be16(boxes[i].y2, &p[6]);
		sys_put_be16(boxes[i].score, &p[8]);
	}

	/* A backed-up stream drops the boxes rather than stall detection. */
	(void)frame_pool_write(frame_id, FRAME_FMT_DETECTIONS, width, height, payload,
			       p - payload, K_NO_WAIT);
}

static void model_input_send(uint16_t frame_id)
{
	struct frame_writer writer;
	uint8_t row[DETECTOR_INPUT_WIDTH * 2];

	frame_writer_init(&writer, frame_id, FRAME_FMT_RGB565_BE, DETECTOR_INPUT_WIDTH,
			  DETECTOR_INPUT_HEIGHT, K_NO_WAIT);

	for (uint16_t y = 0; y < DETECTOR_INPUT_HEIGHT; y++) {
		detector_input_row_rgb565(y, row);
		frame_writer_put(&writer, row, sizeof(row));
	}

	if (frame_writer_finish(&writer)) {
		LOG_WRN("No chunk buffer, model input %u cut short", frame_id);
	}
}

static void frame_detect(struct detection_state_object *state_object,
			 const struct detection_frame_msg *frame)
{
	struct detection_stats *stats = &state_object->stats;
	struct detector_box boxes[CONFIG_APP_DETECTOR_MAX_BOXES];
	int64_t t0 = k_uptime_get();
	int64_t t1;
	int count;

	if (geometry_follow(state_object, frame->width, frame->height) != 0 ||
	    jpeg_decode(slot, frame->len, state_object->scale_shift, detector_sink, NULL) != 0) {
		stats->failed++;
		return;
	}

	t1 = k_uptime_get();
	stats->decode_ms += (uint32_t)(t1 - t0);
	t0 = t1;

	count = detector_run(boxes, ARRAY_SIZE(boxes));
	if (count < 0) {
		stats->failed++;
		return;
	}

	t1 = k_uptime_get();
	stats->infer_ms += (uint32_t)(t1 - t0);
	t0 = t1;

	if (state_object->ai_input) {
		boxes_to_model_input(state_object, boxes, count);
		detections_send(frame->frame_id, boxes, count, DETECTOR_INPUT_WIDTH,
				DETECTOR_INPUT_HEIGHT);
		model_input_send(frame->frame_id);
	} else {
		detections_send(frame->frame_id, boxes, count, frame->width, frame->height);
	}

	stats->send_ms += (uint32_t)(k_uptime_get() - t0);
	stats->detected++;
	stats->last_persons = count;

	result_publish(frame->frame_id, count);
}

static void stats_log_if_due(struct detection_stats *stats)
{
	const int64_t elapsed_ms = k_uptime_get() - stats->window_start_ms;
	const uint32_t detected = MAX(stats->detected, 1);

	if (elapsed_ms < STATS_INTERVAL_MS) {
		return;
	}

	LOG_INF("Detected %u frames in %u ms: decode %u ms, infer %u ms, send %u ms, "
		"%d person(s) last; %u failed, %u too large", stats->detected,
		(uint32_t)elapsed_ms, stats->decode_ms / detected, stats->infer_ms / detected,
		stats->send_ms / detected, stats->last_persons, stats->failed,
		(uint32_t)atomic_clear(&too_large));

	*stats = (struct detection_stats){
		.window_start_ms = k_uptime_get(),
		.last_persons = stats->last_persons,
	};
}

static void config_handle(struct detection_state_object *state_object,
			  const struct detection_msg *msg)
{
	switch (msg->type) {
	case DETECTION_FPS_SET:
		state_object->fps = MIN(msg->fps, CONFIG_APP_DETECTION_FPS_MAX);
		setting_store("detection/fps", state_object->fps);
		status_log(state_object);
		break;
	case DETECTION_THRESHOLD_SET:
		state_object->threshold = MIN(msg->threshold, 100);
		detector_min_score_set(state_object->threshold);
		setting_store("detection/threshold", state_object->threshold);
		status_log(state_object);
		break;
	case DETECTION_AI_INPUT_SET:
		state_object->ai_input = msg->ai_input;
		status_log(state_object);
		break;
	case DETECTION_STATUS:
		status_log(state_object);
		break;
	case DETECTION_RESULT:
		break;
	}
}

static enum smf_state_result active_run(void *obj)
{
	struct detection_state_object *state_object = obj;

	if (state_object->chan == &detection_chan) {
		config_handle(state_object, (const struct detection_msg *)state_object->msg_buf);
	}

	return SMF_EVENT_HANDLED;
}

static void idle_entry(void *obj)
{
	ARG_UNUSED(obj);

	atomic_set(&accepting, true);
}

static enum smf_state_result idle_run(void *obj)
{
	struct detection_state_object *state_object = obj;

	if (state_object->chan == &detection_frame_chan) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_DETECTING]);
		return SMF_EVENT_HANDLED;
	}

	return SMF_EVENT_PROPAGATE;
}

/* Runs to completion, then hands the slot back by returning to STATE_IDLE. */
static void detecting_entry(void *obj)
{
	struct detection_state_object *state_object = obj;
	const uint32_t now_ms = k_uptime_get_32();

	atomic_set(&due_ms, state_object->fps ? now_ms + MSEC_PER_SEC / state_object->fps : now_ms);

	frame_detect(state_object, (const struct detection_frame_msg *)state_object->msg_buf);

	smf_set_state(SMF_CTX(state_object), &states[STATE_IDLE]);
}

static enum smf_state_result disabled_run(void *obj)
{
	struct detection_state_object *state_object = obj;
	const struct detection_msg *msg = (const struct detection_msg *)state_object->msg_buf;

	if (state_object->chan == &detection_chan && msg->type == DETECTION_STATUS) {
		LOG_WRN("Detector unavailable");
	}

	return SMF_EVENT_HANDLED;
}

static void detection_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Detection watchdog expired, channel: %d, thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void detection_thread(void)
{
	struct detection_state_object *state_object = &detection_state;
	int task_wdt_id;
	int err;

	err = settings_subsys_init();
	if (!err) {
		err = settings_load_subtree("detection");
	}
	if (err) {
		LOG_ERR("Loading detection settings, error: %d", err);
	}

	err = detector_init(DETECTOR_INPUT_WIDTH, DETECTOR_INPUT_HEIGHT, 0);
	detector_min_score_set(state_object->threshold);

	task_wdt_id = task_wdt_add(CONFIG_APP_DETECTION_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC,
				   detection_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("task_wdt_add, error: %d", task_wdt_id);
		FATAL_ERROR();
		return;
	}

	state_object->stats.window_start_ms = k_uptime_get();
	smf_set_initial(SMF_CTX(state_object),
			&states[err ? STATE_DISABLED : STATE_ACTIVE]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&detection, &state_object->chan, state_object->msg_buf,
					WAIT);
		if (err == 0) {
			err = smf_run_state(SMF_CTX(state_object));
			if (err) {
				LOG_ERR("smf_run_state, error: %d", err);
				FATAL_ERROR();
				return;
			}
		} else if (err != -ENOMSG) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			FATAL_ERROR();
			return;
		}

		stats_log_if_due(&state_object->stats);
	}
}

K_THREAD_DEFINE(detection_module_thread, CONFIG_APP_DETECTION_THREAD_STACK_SIZE,
		detection_thread, NULL, NULL, NULL, THREAD_PRIORITY, 0, 0);
