/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/settings/settings.h>
#include <zephyr/smf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "lib/frame_pool/frame_pool.h"
#include "stream.h"

LOG_MODULE_REGISTER(stream, CONFIG_APP_STREAM_LOG_LEVEL);

/*
 * Datagram header, big-endian:
 *  0 magic "VS"   u16     12 frame_id   u16
 *  2 version      u8      14 fmt        u8
 *  3 type         u8      15 flags      u8 (bit 0: last chunk)
 *  4 seq          u32     16 width      u16
 *  8 uptime_ms    u32     18 height     u16
 *                         20 frame_size u32
 *                         24 offset     u32
 */
#define STREAM_MAGIC 0x5653
#define STREAM_VERSION 1
#define STREAM_HDR_SIZE 28

#define OPEN_RETRY_INTERVAL_MS (5 * MSEC_PER_SEC)
#define STATS_INTERVAL_MS (CONFIG_APP_STREAM_STATS_INTERVAL_SECONDS * MSEC_PER_SEC)
#define POLL_WAIT K_MSEC(100)

BUILD_ASSERT(STREAM_HDR_SIZE == FRAME_CHUNK_HEADROOM);
BUILD_ASSERT(CONFIG_APP_STREAM_DATAGRAM_SIZE > STREAM_HDR_SIZE);
BUILD_ASSERT(sizeof(CONFIG_APP_STREAM_HOST) <= STREAM_HOST_MAX_LEN + 1);

enum stream_pkt_type {
	STREAM_PKT_FRAME = 1,
	/* Boxes for the frame with the same frame_id; see FRAME_FMT_DETECTIONS. */
	STREAM_PKT_DETECT = 2,
};

ZBUS_CHAN_DEFINE(stream_chan,
		 struct stream_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(stream);

ZBUS_CHAN_ADD_OBS(stream_chan, stream, 0);

enum stream_state {
	STATE_IDLE,
	STATE_STREAMING,
};

struct stream_stats {
	uint32_t sent;
	uint32_t dropped;
	uint32_t bytes;
	uint32_t frames;
	int64_t window_start_ms;
	uint32_t last_kbps;
	uint32_t last_fps_x10;
};

struct stream_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[sizeof(struct stream_msg)];
	char host[STREAM_HOST_MAX_LEN + 1];
	uint16_t port;
	/* net_if index the socket goes out on. */
	int iface;
	struct net_sockaddr_in peer;
	bool peer_valid;
	bool connected;
	bool wanted;
	bool streaming;
	int sock;
	uint32_t seq;
	int64_t next_open_ms;
	uint64_t total_bytes;
	struct stream_stats stats;
};

static void idle_entry(void *obj);
static enum smf_state_result idle_run(void *obj);
static void streaming_entry(void *obj);
static enum smf_state_result streaming_run(void *obj);
static void streaming_exit(void *obj);

static const struct smf_state states[] = {
	[STATE_IDLE] = SMF_CREATE_STATE(idle_entry, idle_run, NULL, NULL, NULL),
	[STATE_STREAMING] = SMF_CREATE_STATE(streaming_entry, streaming_run, streaming_exit,
					     NULL, NULL),
};

static struct stream_state_object stream_state = {
	.host = CONFIG_APP_STREAM_HOST,
	.port = CONFIG_APP_STREAM_PORT,
	.wanted = true,
	.sock = -1,
};

static int settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	ssize_t n;

	if (strcmp(key, "host") == 0) {
		if (len > STREAM_HOST_MAX_LEN) {
			return -EINVAL;
		}
		n = read_cb(cb_arg, stream_state.host, len);
		if (n < 0) {
			return n;
		}
		stream_state.host[n] = '\0';
		return 0;
	}

	if (strcmp(key, "port") == 0) {
		if (len != sizeof(stream_state.port)) {
			return -EINVAL;
		}
		n = read_cb(cb_arg, &stream_state.port, len);
		return n < 0 ? n : 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(stream, "stream", NULL, settings_set, NULL, NULL);

static void endpoint_store(struct stream_state_object *obj, const struct stream_msg *msg)
{
	int err;

	memcpy(obj->host, msg->host, sizeof(obj->host));
	obj->peer_valid = false;
	obj->host[STREAM_HOST_MAX_LEN] = '\0';
	obj->port = msg->port;

	LOG_INF("Endpoint %s:%u", obj->host, obj->port);

	err = settings_save_one("stream/host", obj->host, strlen(obj->host));
	if (!err) {
		err = settings_save_one("stream/port", &obj->port, sizeof(obj->port));
	}
	if (err) {
		LOG_ERR("settings_save_one, error: %d", err);
	}
}

static void status_log(const struct stream_state_object *obj)
{
	const char *state = !obj->streaming ? "Idle" : obj->sock >= 0 ? "Streaming" : "Connecting";

	LOG_INF("%s to %s:%u, %s, network %s", state, obj->host[0] ? obj->host : "<none>",
		obj->port, obj->wanted ? "started" : "stopped", obj->connected ? "up" : "down");
	LOG_INF("Last %u kbit/s, %u.%u fps; %llu bytes sent since boot",
		obj->stats.last_kbps, obj->stats.last_fps_x10 / 10, obj->stats.last_fps_x10 % 10,
		obj->total_bytes);
}

/* Keeps the last good address so moving to another interface does not hinge on DNS. */
static int peer_resolve(struct stream_state_object *obj)
{
	struct zsock_addrinfo hints = {
		.ai_family = NET_AF_INET,
		.ai_socktype = NET_SOCK_DGRAM,
	};
	struct zsock_addrinfo *res;
	char port[6];
	int err;

	snprintk(port, sizeof(port), "%u", obj->port);

	err = zsock_getaddrinfo(obj->host, port, &hints, &res);
	if (err == 0) {
		memcpy(&obj->peer, res->ai_addr, MIN(res->ai_addrlen, sizeof(obj->peer)));
		obj->peer_valid = true;
		zsock_freeaddrinfo(res);
		return 0;
	}

	if (obj->peer_valid) {
		LOG_WRN("zsock_getaddrinfo %s, error: %d; using the last address", obj->host,
			err);
		return 0;
	}

	LOG_WRN("zsock_getaddrinfo %s, error: %d", obj->host, err);
	return -EHOSTUNREACH;
}

static int socket_open(struct stream_state_object *obj)
{
	struct zsock_timeval send_timeout = {
		.tv_sec = CONFIG_APP_STREAM_SEND_TIMEOUT_MS / MSEC_PER_SEC,
		.tv_usec = (CONFIG_APP_STREAM_SEND_TIMEOUT_MS % MSEC_PER_SEC) * USEC_PER_MSEC,
	};
	struct net_if *iface = net_if_get_by_index(obj->iface);
	char iface_name[CONFIG_NET_INTERFACE_NAME_LEN + 1] = "?";
	int sock;
	int err;

	/* connect() picks the source address, and with it the interface, from the default
	 * route, so steer it there before resolving and connecting.
	 */
	if (iface != NULL) {
		net_if_set_default(iface);
		(void)net_if_get_name(iface, iface_name, sizeof(iface_name));
	}

	err = peer_resolve(obj);
	if (err) {
		return err;
	}

	sock = zsock_socket(NET_AF_INET, NET_SOCK_DGRAM, NET_IPPROTO_UDP);
	if (sock < 0) {
		err = -errno;
		LOG_ERR("zsock_socket, error: %d", err);
		return err;
	}

	err = zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO,
			       &send_timeout, sizeof(send_timeout));
	if (err) {
		err = -errno;
		LOG_ERR("zsock_setsockopt SO_SNDTIMEO, error: %d", err);
		goto close;
	}

	/* connect() pins the peer so each send skips the address lookup. */
	err = zsock_connect(sock, (struct net_sockaddr *)&obj->peer, sizeof(obj->peer));
	if (err) {
		err = -errno;
		LOG_ERR("zsock_connect, error: %d", err);
		goto close;
	}

	LOG_INF("Streaming to %s:%u over %s", obj->host, obj->port, iface_name);
	return sock;

close:
	zsock_close(sock);
	return err;
}

static void stats_reset(struct stream_stats *stats)
{
	*stats = (struct stream_stats){
		.window_start_ms = k_uptime_get(),
		.last_kbps = stats->last_kbps,
		.last_fps_x10 = stats->last_fps_x10,
	};
}

static void stats_log_if_due(struct stream_stats *stats)
{
	int64_t elapsed_ms = k_uptime_get() - stats->window_start_ms;

	if (elapsed_ms < STATS_INTERVAL_MS) {
		return;
	}

	stats->last_fps_x10 = (uint32_t)((stats->frames * 10ULL * MSEC_PER_SEC) / elapsed_ms);
	stats->last_kbps = (uint32_t)((stats->bytes * 8ULL) / elapsed_ms);

	LOG_INF("Sent %u datagrams, %u dropped, %u kbit/s, %u.%u fps",
		stats->sent, stats->dropped, stats->last_kbps,
		stats->last_fps_x10 / 10, stats->last_fps_x10 % 10);

	stats_reset(stats);
}

static void put_header(uint8_t *buf, uint8_t type, uint32_t seq, const struct frame_chunk *c)
{
	sys_put_be16(STREAM_MAGIC, &buf[0]);
	buf[2] = STREAM_VERSION;
	buf[3] = type;
	sys_put_be32(seq, &buf[4]);
	sys_put_be32((uint32_t)k_uptime_get(), &buf[8]);
	sys_put_be16(c->frame_id, &buf[12]);
	buf[14] = c->fmt;
	buf[15] = c->flags;
	sys_put_be16(c->width, &buf[16]);
	sys_put_be16(c->height, &buf[18]);
	sys_put_be32(c->frame_size, &buf[20]);
	sys_put_be32(c->offset, &buf[24]);
}

static void send_datagram(struct stream_state_object *obj, const uint8_t *buf, size_t len)
{
	ssize_t sent = zsock_send(obj->sock, buf, len, 0);

	if (sent < 0) {
		if (errno != EAGAIN && errno != ENOMEM) {
			LOG_WRN("zsock_send, error: %d", -errno);
		}
		obj->stats.dropped++;
	} else {
		obj->stats.sent++;
		obj->stats.bytes += sent;
		obj->total_bytes += sent;
	}

	obj->seq++;
}

/* Chunks are drained even while offline so the camera never stalls. */
static void frames_tick(struct stream_state_object *obj, bool online)
{
	struct frame_chunk chunk;

	if (frame_pool_get(&chunk, POLL_WAIT) != 0) {
		return;
	}

	if (online) {
		const bool detections = (chunk.fmt == FRAME_FMT_DETECTIONS);

		put_header(chunk.buf, detections ? STREAM_PKT_DETECT : STREAM_PKT_FRAME, obj->seq,
			   &chunk);
		send_datagram(obj, chunk.buf, STREAM_HDR_SIZE + chunk.len);

		if (!detections && (chunk.flags & FRAME_CHUNK_LAST)) {
			obj->stats.frames++;
		}
	}

	frame_pool_release(&chunk);
}

static void stream_tick(struct stream_state_object *obj)
{
	bool online;

	if (obj->streaming && obj->sock < 0 && k_uptime_get() >= obj->next_open_ms) {
		obj->sock = socket_open(obj);
		if (obj->sock < 0) {
			obj->next_open_ms = k_uptime_get() + OPEN_RETRY_INTERVAL_MS;
		} else {
			stats_reset(&obj->stats);
		}
	}

	online = obj->streaming && obj->sock >= 0;

	frames_tick(obj, online);

	if (online) {
		stats_log_if_due(&obj->stats);
	}
}

/* Returns true when an open socket must be reopened. */
static bool msg_apply(struct stream_state_object *obj)
{
	const struct stream_msg *msg = (const struct stream_msg *)obj->msg_buf;
	bool moved;

	switch (msg->type) {
	case STREAM_CONNECT:
		moved = obj->connected && obj->iface != msg->iface;
		obj->connected = true;
		obj->iface = msg->iface;
		if (obj->host[0] == '\0') {
			LOG_WRN("No endpoint, set one with: stream endpoint <host> [port]");
		}
		return moved;
	case STREAM_DISCONNECT:
		obj->connected = false;
		break;
	case STREAM_START:
		LOG_INF("Started");
		obj->wanted = true;
		break;
	case STREAM_STOP:
		LOG_INF("Stopped");
		obj->wanted = false;
		break;
	case STREAM_ENDPOINT:
		endpoint_store(obj, msg);
		return true;
	case STREAM_STATUS:
		status_log(obj);
		break;
	}

	return false;
}

static bool can_stream(const struct stream_state_object *obj)
{
	return obj->connected && obj->wanted && obj->host[0] != '\0';
}

static void idle_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result idle_run(void *obj)
{
	struct stream_state_object *state_object = obj;

	(void)msg_apply(state_object);

	if (can_stream(state_object)) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_STREAMING]);
	}

	return SMF_EVENT_HANDLED;
}

static void streaming_entry(void *obj)
{
	struct stream_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	state_object->streaming = true;
	state_object->next_open_ms = k_uptime_get();
}

static enum smf_state_result streaming_run(void *obj)
{
	struct stream_state_object *state_object = obj;
	const bool reopen = msg_apply(state_object);

	if (!can_stream(state_object)) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_IDLE]);
	} else if (reopen) {
		if (state_object->sock >= 0) {
			zsock_close(state_object->sock);
			state_object->sock = -1;
		}
		state_object->next_open_ms = k_uptime_get();
	}

	return SMF_EVENT_HANDLED;
}

static void streaming_exit(void *obj)
{
	struct stream_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	state_object->streaming = false;

	if (state_object->sock >= 0) {
		zsock_close(state_object->sock);
		state_object->sock = -1;
	}
}

static void stream_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Stream watchdog expired, channel: %d, thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void stream_module(void)
{
	int err;
	int task_wdt_id;

	err = settings_subsys_init();
	if (!err) {
		err = settings_load_subtree("stream");
	}
	if (err) {
		LOG_ERR("Loading stream settings, error: %d", err);
	}

	task_wdt_id = task_wdt_add(CONFIG_APP_STREAM_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC,
				   stream_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("task_wdt_add, error: %d", task_wdt_id);
		FATAL_ERROR();
		return;
	}

	smf_set_initial(SMF_CTX(&stream_state), &states[STATE_IDLE]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			FATAL_ERROR();
			return;
		}

		/* frames_tick() blocks on the frame pool, so zbus is only polled. */
		err = zbus_sub_wait_msg(&stream, &stream_state.chan, stream_state.msg_buf, K_NO_WAIT);
		if (err == 0) {
			err = smf_run_state(SMF_CTX(&stream_state));
			if (err) {
				LOG_ERR("smf_run_state, error: %d", err);
				FATAL_ERROR();
				return;
			}
		} else if (err != -ENOMSG && err != -EAGAIN) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			FATAL_ERROR();
			return;
		}

		stream_tick(&stream_state);
	}
}

K_THREAD_DEFINE(stream_module_thread, CONFIG_APP_STREAM_THREAD_STACK_SIZE,
		stream_module, NULL, NULL, NULL, 10, 0, 0);
