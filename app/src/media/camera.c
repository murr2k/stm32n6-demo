/*
 * Camera capture. See camera.h.
 *
 * Four frame buffers in PSRAM are imported into the video layer (its own pool
 * lives in internal RAM, which has no room for frames). The capture thread
 * dequeues each finished frame, makes it the newest, and gives the previous
 * newest back to the DCMIPP unless the UI thread is still copying it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video.h>
#include <zephyr/video/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "camera.h"
#include "platform/gfx.h"
#include "platform/psram.h"

LOG_MODULE_REGISTER(camera, LOG_LEVEL_INF);

/* DCMIPP, newest, the UI's, the NPU's. CONFIG_VIDEO_BUFFER_POOL_NUM_MAX
 * limits it: a fifth needs that raised too. The readers hold a frame only
 * while they copy or crop it, so the DCMIPP is rarely left without one. */
#define NBUF       4
#define FRAME_SIZE (CAM_W * CAM_H * 2)

static const struct device *const cam = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));
static const struct device *const sensor = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(imx335));

static uint16_t idx[NBUF];
static bool configured;
static volatile bool running;
static struct camera_status status;
static struct k_spinlock lock;
static struct video_buffer *newest, *held, *held_nn;
static uint32_t newest_seq;
static bool newest_seen;
static K_SEM_DEFINE(go, 0, 1);
static K_SEM_DEFINE(nn_frame, 0, 1);

static void give_back(struct video_buffer *vb)
{
	vb->type = VIDEO_BUF_TYPE_OUTPUT;
	video_enqueue(cam, vb);
}

static int configure(void)
{
	struct video_format fmt = {
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width = CAM_W,
		.height = CAM_H,
		.type = VIDEO_BUF_TYPE_OUTPUT,
	};
	struct video_frmival fi = {.numerator = 1, .denominator = 30};

	if (!device_is_ready(cam)) {
		status.error = "camera pipe not ready (is the module in the CSI connector?)";
		return -ENODEV;
	}
	/* the scaled size is set as the compose rectangle, then the format */
	if (video_set_compose_format(cam, &fmt) != 0) {
		status.error = "the pipe refused the format";
		return -EIO;
	}
	video_set_frmival(cam, &fi); /* best effort: the sensor may keep its own rate */
	for (int i = 0; i < NBUF; i++) {
		uint8_t *mem = psram_alloc(FRAME_SIZE, 64);

		if (mem == NULL || video_import_buffer(mem, FRAME_SIZE, &idx[i]) != 0) {
			status.error = "no memory for the frame buffers";
			return -ENOMEM;
		}
	}
	configured = true;
	status.ready = true;
	LOG_INF("camera: %dx%d RGB565, %d buffers in PSRAM", CAM_W, CAM_H, NBUF);
	return 0;
}

static volatile bool want;

/* Starting and stopping talk to the sensor over I2C and wait for the pipe: on
 * the UI thread that cost a frame each time the Camera screen opened or
 * closed. The capture thread does it; these only ask.
 */
int camera_start(void)
{
	want = true;
	k_sem_give(&go);
	return 0;
}

void camera_stop(void)
{
	want = false;
	k_sem_give(&go);
}

static int do_start(void)
{
	if (running) {
		return 0;
	}
	if (!configured && configure() != 0) {
		return -EIO;
	}
	for (int i = 0; i < NBUF; i++) {
		struct video_buffer vb = {.index = idx[i], .type = VIDEO_BUF_TYPE_OUTPUT};

		video_enqueue(cam, &vb);
	}
	newest = held = NULL;
	status.frames = 0;
	status.fps_x10 = 0;
	if (video_stream_start(cam, VIDEO_BUF_TYPE_OUTPUT) != 0) {
		status.error = "the stream did not start";
		return -EIO;
	}
	status.error = NULL;
	running = true;
	status.running = true;
	return 0;
}

static void do_stop(void)
{
	struct video_buffer *vb;

	if (!running) {
		return;
	}
	running = false;
	video_stream_stop(cam, VIDEO_BUF_TYPE_OUTPUT);
	video_flush(cam, true);
	while (video_dequeue(cam, &vb, K_NO_WAIT) == 0) {
	}
	k_spinlock_key_t key = k_spin_lock(&lock);

	newest = held = held_nn = NULL;
	status.running = false;
	k_spin_unlock(&lock, key);
}

const uint16_t *camera_acquire(bool *fresh)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const uint16_t *p = NULL;

	held = newest;
	if (held) {
		p = (const uint16_t *)held->buffer;
	}
	*fresh = held && !newest_seen;
	newest_seen = true;
	k_spin_unlock(&lock, key);
	return p;
}

void camera_release(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct video_buffer *vb = held;

	held = NULL;
	if (vb == newest || vb == held_nn) {
		vb = NULL; /* the capture thread or the other reader gives it back */
	}
	k_spin_unlock(&lock, key);
	if (vb && running) {
		give_back(vb);
	}
}

int camera_wait_frame(k_timeout_t timeout)
{
	return k_sem_take(&nn_frame, timeout);
}

const uint16_t *camera_acquire_nn(uint32_t *seq)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const uint16_t *p = NULL;

	held_nn = newest;
	if (held_nn) {
		p = (const uint16_t *)held_nn->buffer;
	}
	*seq = newest_seq;
	k_spin_unlock(&lock, key);
	return p;
}

void camera_release_nn(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct video_buffer *vb = held_nn;

	held_nn = NULL;
	if (vb == newest || vb == held) {
		vb = NULL;
	}
	k_spin_unlock(&lock, key);
	if (vb && running) {
		give_back(vb);
	}
}

/* the lens cap: on at every start, not stored */
static volatile bool snap_allowed;

void camera_snapshot_allow(bool on)
{
	snap_allowed = on;
}

bool camera_snapshot_allowed(void)
{
	return snap_allowed;
}

int camera_snapshot(int step, int settle, const uint16_t **pix, int *w, int *h)
{
	static uint16_t *snap;
	bool started_here = !want;
	bool fresh;
	int ret = 0;

	if (!snap_allowed) {
		return -EACCES;
	}
	step = CLAMP(step, 1, 8);
	settle = CLAMP(settle, 1, 300);
	if (snap == NULL) {
		snap = psram_alloc(FRAME_SIZE, 64);
		if (snap == NULL) {
			return -ENOMEM;
		}
	}
	if (started_here) {
		camera_start();
	}
	/* frames counts from the start; with the camera already running, wait for
	 * `settle` more */
	uint32_t f0 = running ? status.frames : 0;
	uint32_t t0 = k_uptime_get_32();

	while (!running || status.frames < f0 + (uint32_t)settle) {
		if (k_uptime_get_32() - t0 > 3000u + (uint32_t)settle * 100u) {
			ret = -ETIMEDOUT;
			goto out;
		}
		k_msleep(20);
	}
	const uint16_t *f = camera_acquire(&fresh);

	if (f == NULL) {
		camera_release();
		ret = -EAGAIN;
		goto out;
	}
	/* written by the DCMIPP behind the cache; this reader never writes it */
	sys_cache_data_invd_range((void *)f, FRAME_SIZE);
	*w = CAM_W / step;
	*h = CAM_H / step;
	for (int y = 0; y < *h; y++) {
		const uint16_t *src = f + (size_t)y * step * CAM_W;
		uint16_t *dst = snap + (size_t)y * *w;

		for (int x = 0; x < *w; x++) {
			dst[x] = src[x * step];
		}
	}
	camera_release();
	*pix = snap;
out:
	if (started_here) {
		camera_stop();
	}
	return ret;
}

void camera_get_status(struct camera_status *st)
{
	*st = status;
	if (sensor && device_is_ready(sensor)) {
		struct video_control c = {.id = VIDEO_CID_ANALOGUE_GAIN};

		if (video_get_ctrl(sensor, &c) == 0) {
			st->gain_mdb = c.val;
		}
		c.id = VIDEO_CID_EXPOSURE;
		if (video_get_ctrl(sensor, &c) == 0) {
			st->exposure = c.val;
		}
	}
}

static void capture_thread(void *p1, void *p2, void *p3)
{
	uint32_t t_mark = 0, n_mark = 0;

	for (;;) {
		struct video_buffer *vb, *old;

		if (want && !running) {
			if (do_start() != 0) {
				LOG_ERR("%s", status.error ? status.error : "camera did not start");
				want = false;
			}
		} else if (!want && running) {
			do_stop();
		}
		if (!running) {
			k_sem_take(&go, K_FOREVER);
			continue;
		}
		if (video_dequeue(cam, &vb, K_MSEC(100)) != 0) {
			continue;
		}
		k_spinlock_key_t key = k_spin_lock(&lock);

		old = newest;
		newest = vb;
		newest_seq++;
		newest_seen = false;
		if (old == held || old == held_nn) {
			old = NULL; /* its reader gives it back when it lets go */
		}
		k_spin_unlock(&lock, key);
		if (old) {
			give_back(old);
		}
		k_sem_give(&nn_frame);

		uint32_t now = gfx_now_us();

		if (status.frames++ == 0) {
			t_mark = now, n_mark = 0;
		} else if (status.frames - n_mark >= 30) {
			status.fps_x10 = (uint32_t)((uint64_t)(status.frames - n_mark) * 10000000 / (now - t_mark));
			t_mark = now, n_mark = status.frames;
		}
	}
}
/* Below the UI and the player, above the network services. */
K_THREAD_DEFINE(camera_tid, 3072, capture_thread, NULL, NULL, NULL, 4, 0, 0);
