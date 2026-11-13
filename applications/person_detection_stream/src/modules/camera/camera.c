/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "camera.h"
#include "lib/frame_pool/frame_pool.h"
#if defined(CONFIG_APP_DETECTION)
#include "modules/detection/detection.h"
#endif

LOG_MODULE_REGISTER(camera, CONFIG_APP_CAMERA_LOG_LEVEL);

/*
 * The capture thread only collects JPEG frames from the driver and hands the newest
 * one over. The process thread offers it to detection and slices it into chunks as is.
 */
#define CAPTURE_THREAD_PRIORITY 4
#define PROCESS_THREAD_PRIORITY 9
#define WAIT_MS MSEC_PER_SEC
#define WAIT K_MSEC(WAIT_MS)
#define STATS_INTERVAL_MS (CONFIG_APP_CAMERA_STATS_INTERVAL_SECONDS * MSEC_PER_SEC)
#define PIXEL_FORMAT VIDEO_PIX_FMT_JPEG

struct camera_frame {
	struct video_buffer *vbuf;
	uint32_t len;
	uint16_t width;
	uint16_t height;
};

struct profile_cfg {
	uint16_t width;
	uint16_t height;
	uint8_t quality;
};

static const struct profile_cfg profiles[] = {
	[CAMERA_PROFILE_LOW] = { CONFIG_APP_CAMERA_WIDTH, CONFIG_APP_CAMERA_HEIGHT,
				 CONFIG_APP_CAMERA_JPEG_QUALITY },
	[CAMERA_PROFILE_HIGH] = { CONFIG_APP_CAMERA_HIGH_WIDTH, CONFIG_APP_CAMERA_HIGH_HEIGHT,
				  CONFIG_APP_CAMERA_HIGH_JPEG_QUALITY },
};

static const char *const profile_names[] = {
	[CAMERA_PROFILE_AUTO] = "auto",
	[CAMERA_PROFILE_LOW] = "low",
	[CAMERA_PROFILE_HIGH] = "high",
};

static const struct device *const camera = DEVICE_DT_GET_ONE(nordic_arducam_mega);

/* Holds at most one frame: a newer frame replaces one the process thread has not taken yet. */
K_MSGQ_DEFINE(pending_q, sizeof(struct camera_frame), 1, 4);

static atomic_t captured;
static atomic_t replaced;
static atomic_t invalid;
static atomic_t fps_limit = ATOMIC_INIT(CONFIG_APP_CAMERA_FPS_LIMIT);
static atomic_t streaming = ATOMIC_INIT(true);
static atomic_t profile_set = ATOMIC_INIT(CAMERA_PROFILE_AUTO);
static atomic_t profile_hint = ATOMIC_INIT(CAMERA_PROFILE_LOW);
/* Only touched by the capture thread, which owns the camera. */
static enum camera_profile profile_active;

static void camera_callback(const struct zbus_channel *chan);

ZBUS_LISTENER_DEFINE(camera_listener, camera_callback);

ZBUS_CHAN_DEFINE(camera_chan,
		 struct camera_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_CHAN_ADD_OBS(camera_chan, camera_listener, 0);

static int settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t value;
	ssize_t n;

	if (len != sizeof(value)) {
		return -EINVAL;
	}

	if (strcmp(key, "fps") == 0) {
		n = read_cb(cb_arg, &value, len);
		if (n < 0) {
			return n;
		}
		atomic_set(&fps_limit, MIN(value, CONFIG_APP_CAMERA_FPS_MAX));
		return 0;
	}

	if (strcmp(key, "profile") == 0) {
		n = read_cb(cb_arg, &value, len);
		if (n < 0) {
			return n;
		}
		atomic_set(&profile_set, MIN(value, CAMERA_PROFILE_HIGH));
		return 0;
	}

	return -ENOENT;
}

/* The hint is a ceiling, so HIGH never reaches LTE. HIGH needs the board to define one. */
static enum camera_profile profile_wanted(void)
{
	enum camera_profile profile = (enum camera_profile)atomic_get(&profile_hint);

	if (atomic_get(&profile_set) == CAMERA_PROFILE_LOW) {
		profile = CAMERA_PROFILE_LOW;
	}

	if (profile == CAMERA_PROFILE_HIGH && CONFIG_APP_CAMERA_HIGH_WIDTH == 0) {
		profile = CAMERA_PROFILE_LOW;
	}

	return profile;
}

SETTINGS_STATIC_HANDLER_DEFINE(camera, "camera", NULL, settings_set, NULL, NULL);

static void status_log(void)
{
	const uint8_t fps = (uint8_t)atomic_get(&fps_limit);
	char text[4] = "max";

	if (fps) {
		snprintk(text, sizeof(text), "%u", fps);
	}

	LOG_INF("Camera fps %s, video stream %s, profile %s (%ux%u)", text,
		atomic_get(&streaming) ? "on" : "off",
		profile_names[atomic_get(&profile_set)], profiles[profile_wanted()].width,
		profiles[profile_wanted()].height);
}

static void camera_callback(const struct zbus_channel *chan)
{
	const struct camera_msg *msg = zbus_chan_const_msg(chan);
	uint8_t value;
	uint8_t fps;
	int err;

	switch (msg->type) {
	case CAMERA_FPS_SET:
		fps = MIN(msg->fps, CONFIG_APP_CAMERA_FPS_MAX);
		atomic_set(&fps_limit, fps);
		err = settings_save_one("camera/fps", &fps, sizeof(fps));
		if (err) {
			LOG_ERR("settings_save_one camera/fps, error: %d", err);
		}
		status_log();
		break;
	case CAMERA_STREAM_SET:
		atomic_set(&streaming, msg->stream);
		status_log();
		break;
	case CAMERA_STATUS:
		status_log();
		break;
	case CAMERA_PROFILE_SET:
		value = MIN(msg->profile, CAMERA_PROFILE_HIGH);
		atomic_set(&profile_set, value);
		err = settings_save_one("camera/profile", &value, sizeof(value));
		if (err) {
			LOG_ERR("settings_save_one camera/profile, error: %d", err);
		}
		status_log();
		break;
	case CAMERA_PROFILE_HINT:
		atomic_set(&profile_hint, MIN(msg->profile, CAMERA_PROFILE_HIGH));
		break;
	}
}

static void camera_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Camera watchdog expired, channel: %d, thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static int wdt_add(void)
{
	int id = task_wdt_add(CONFIG_APP_CAMERA_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC,
			      camera_wdt_callback, (void *)k_current_get());

	if (id < 0) {
		LOG_ERR("task_wdt_add, error: %d", id);
		FATAL_ERROR();
	}

	return id;
}

static void wdt_feed(int id)
{
	int err = task_wdt_feed(id);

	if (err) {
		LOG_ERR("task_wdt_feed, error: %d", err);
		FATAL_ERROR();
	}
}

static void buffer_return(struct video_buffer *vbuf)
{
	vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
	(void)video_enqueue(camera, vbuf);
}

static void frame_return(struct camera_frame *frame)
{
	buffer_return(frame->vbuf);
}

static void hand_over(const struct camera_frame *frame)
{
	struct camera_frame stale;

	atomic_inc(&captured);

	while (k_msgq_put(&pending_q, frame, K_NO_WAIT) != 0) {
		if (k_msgq_get(&pending_q, &stale, K_NO_WAIT) == 0) {
			frame_return(&stale);
			atomic_inc(&replaced);
		}
	}
}

static int profile_apply(enum camera_profile profile)
{
	const struct profile_cfg *cfg = &profiles[profile];
	struct video_format fmt = {
		.type = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = PIXEL_FORMAT,
		.width = cfg->width,
		.height = cfg->height,
	};
	struct video_control quality = {
		.id = VIDEO_CID_JPEG_COMPRESSION_QUALITY,
		.val = cfg->quality,
	};
	int err;

	err = video_set_format(camera, &fmt);
	if (err) {
		LOG_ERR("video_set_format %ux%u, error: %d", cfg->width, cfg->height, err);
		return err;
	}

	err = video_set_ctrl(camera, &quality);
	if (err) {
		LOG_WRN("Setting JPEG quality, error: %d", err);
	}

	profile_active = profile;

	LOG_INF("Streaming %s %ux%u, %s profile", VIDEO_FOURCC_TO_STR(PIXEL_FORMAT),
		cfg->width, cfg->height, profile_names[profile]);

	return 0;
}

/* Only between frames: a format change during a frame read corrupts the camera FIFO. */
static void profile_switch(enum camera_profile profile)
{
	int err;

	err = video_stream_stop(camera, VIDEO_BUF_TYPE_OUTPUT);
	if (err) {
		LOG_ERR("video_stream_stop, error: %d", err);
		return;
	}

	/* Frames already captured have the old size; hand their buffers back. */
	(void)video_flush(camera, false);

	if (profile_apply(profile) != 0) {
		(void)profile_apply(profile_active);
	}

	err = video_stream_start(camera, VIDEO_BUF_TYPE_OUTPUT);
	if (err) {
		LOG_ERR("video_stream_start, error: %d", err);
	}
}

static int camera_start(void)
{
	int err;

	if (!device_is_ready(camera)) {
		LOG_ERR("Camera not ready");
		return -ENODEV;
	}

	err = profile_apply(profile_wanted());
	if (err) {
		return err;
	}

	for (int i = 0; i < CONFIG_APP_CAMERA_BUFFER_COUNT; i++) {
		struct video_buffer *vbuf = video_buffer_alloc(CONFIG_APP_CAMERA_BUFFER_SIZE,
							       K_NO_WAIT);

		if (vbuf == NULL) {
			LOG_ERR("video_buffer_alloc failed for buffer %d", i);
			return -ENOMEM;
		}
		buffer_return(vbuf);
	}

	err = video_stream_start(camera, VIDEO_BUF_TYPE_OUTPUT);
	if (err) {
		LOG_ERR("video_stream_start, error: %d", err);
		return err;
	}

	return 0;
}

/* Returns the length up to and including the last EOI marker, or 0 if this is not a JPEG. */
static uint32_t jpeg_length(const uint8_t *data, uint32_t len)
{
	if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
		return 0;
	}

	for (uint32_t i = len - 1; i >= 3; i--) {
		if (data[i - 1] == 0xFF && data[i] == 0xD9) {
			return i + 1;
		}
	}

	return 0;
}

/* A frame larger than one buffer is split by the driver and both halves fail this check. */
static void collect(struct video_buffer *vbuf)
{
	struct camera_frame frame = {
		.vbuf = vbuf,
		.len = jpeg_length(vbuf->buffer, vbuf->bytesused),
		.width = profiles[profile_active].width,
		.height = profiles[profile_active].height,
	};

	if (frame.len == 0) {
		atomic_inc(&invalid);
		buffer_return(vbuf);
		return;
	}

	hand_over(&frame);
}

static void capture_thread(void)
{
	int wdt_id;

	if (camera_start()) {
		return;
	}

	wdt_id = wdt_add();

	while (true) {
		struct video_buffer *vbuf;

		wdt_feed(wdt_id);

		if (profile_wanted() != profile_active) {
			profile_switch(profile_wanted());
		}

		if (video_dequeue(camera, &vbuf, WAIT) == 0) {
			collect(vbuf);
		}
	}
}

struct process_stats {
	int64_t window_start_ms;
	uint32_t frames;
	uint32_t offered;
	uint32_t bytes;
	uint32_t send_ms;
};

static void frame_send(uint16_t frame_id, const struct camera_frame *frame,
		       struct process_stats *stats)
{
	const uint8_t *jpeg = frame->vbuf->buffer;
	const int64_t start_ms = k_uptime_get();

#if defined(CONFIG_APP_DETECTION)
	if (detection_frame_offer(jpeg, frame->len, frame_id, frame->width, frame->height)) {
		stats->offered++;
	}
#endif

	if (atomic_get(&streaming) &&
	    frame_pool_write(frame_id, FRAME_FMT_JPEG, frame->width, frame->height, jpeg,
			     frame->len, WAIT) != 0) {
		LOG_WRN("No chunk buffer, frame %u cut short", frame_id);
	}

	stats->send_ms += (uint32_t)(k_uptime_get() - start_ms);
	stats->bytes += frame->len;
	stats->frames++;
}

static void stats_log_if_due(struct process_stats *stats)
{
	const int64_t elapsed_ms = k_uptime_get() - stats->window_start_ms;
	const uint32_t frames = MAX(stats->frames, 1);

	if (elapsed_ms < STATS_INTERVAL_MS) {
		return;
	}

	LOG_INF("Captured %u, sent %u, replaced %u, invalid %u frames in %u ms",
		(uint32_t)atomic_clear(&captured), stats->frames,
		(uint32_t)atomic_clear(&replaced), (uint32_t)atomic_clear(&invalid),
		(uint32_t)elapsed_ms);

	LOG_INF("Per frame: %u B JPEG, send %u ms; %u taken for detection",
		stats->bytes / frames, stats->send_ms / frames, stats->offered);

	*stats = (struct process_stats){ .window_start_ms = k_uptime_get() };
}

/* Sleeps until the next frame is due, so the one processed is the freshest capture. */
static bool frame_due(int64_t next_ms)
{
	const int64_t now_ms = k_uptime_get();

	if (now_ms >= next_ms) {
		return true;
	}

	k_sleep(K_MSEC(MIN(next_ms - now_ms, WAIT_MS)));

	return false;
}

static void process_thread(void)
{
	struct process_stats stats = { .window_start_ms = k_uptime_get() };
	uint16_t frame_id = 0;
	int64_t next_ms = 0;
	int wdt_id;
	int err;

	err = settings_subsys_init();
	if (!err) {
		/* Stored by builds that ran detection in this module. */
		(void)settings_delete("camera/detect_fps");
		(void)settings_delete("camera/threshold");
		err = settings_load_subtree("camera");
	}
	if (err) {
		LOG_ERR("Loading camera settings, error: %d", err);
	}

	wdt_id = wdt_add();

	while (true) {
		struct camera_frame frame;

		wdt_feed(wdt_id);

		if (frame_due(next_ms) && k_msgq_get(&pending_q, &frame, WAIT) == 0) {
			const uint32_t fps = atomic_get(&fps_limit);

			next_ms = fps ? k_uptime_get() + MSEC_PER_SEC / fps : 0;

			frame_send(frame_id++, &frame, &stats);
			frame_return(&frame);
		}

		stats_log_if_due(&stats);
	}
}

K_THREAD_DEFINE(camera_capture_thread, CONFIG_APP_CAMERA_THREAD_STACK_SIZE,
		capture_thread, NULL, NULL, NULL, CAPTURE_THREAD_PRIORITY, 0, 0);

K_THREAD_DEFINE(camera_process_thread, CONFIG_APP_CAMERA_PROCESS_THREAD_STACK_SIZE,
		process_thread, NULL, NULL, NULL, PROCESS_THREAD_PRIORITY, 0, 0);
