/*
 * Camera: the IMX335 live, demosaiced and scaled by the DCMIPP, exposure and
 * colour by ST's ISP, copied to the display by DMA2D; and hand tracking on
 * the NPU over it (nn/hands.c), with the 21 points, the bones between them
 * and the gesture they make.
 *
 * While this screen is shown the display lends its second frame buffer to the
 * NPU and runs single-buffered (gfx.h): every frame starts at the top of the
 * panel's refresh, and the picture, the first thing drawn and copied by DMA2D
 * about six times faster than the panel scans, stays ahead of the scanout.
 * The view tells LVGL it covers its area, so nothing is painted under it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include <lvgl_private.h> /* lv_cover_check_info_t */
#include <zephyr/kernel.h>

#include "media/camera.h"
#include "media/jpegdec.h"
#include "nn/hands.h"
#include "platform/gfx.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define PX     0
#define PY     0
#define SIDE_X (CAM_W + 16)

static lv_obj_t *view, *l_cap[HANDS_MAX], *l_gesture[HANDS_MAX], *l_npu, *l_track, *l_cam, *l_exp, *l_state;
static uint32_t copy_us, last_frame_ms, frame_gap_ms;
static struct hands_result hands; /* the copy the overlay draws */
/* each slot's colour, on the skeleton and on its gesture */
static const uint32_t hand_color[HANDS_MAX] = {C_AZURE, C_VIOLET};

static void overlay(const struct raster *r)
{
	int ox = r->box.x1, oy = r->box.y1;

	for (int h = 0; h < HANDS_MAX; h++) {
		const struct hand *p = &hands.hand[h];

		if (!p->valid) {
			continue;
		}
		for (int i = 0; i < hand_n_bones; i++) {
			int a = hand_bones[i][0], b = hand_bones[i][1];

			raster_line(r, (int)((ox + p->x[a]) * RQ_ONE), (int)((oy + p->y[a]) * RQ_ONE),
				    (int)((ox + p->x[b]) * RQ_ONE), (int)((oy + p->y[b]) * RQ_ONE), hand_color[h], 20,
				    230);
		}
		for (int i = 0; i < HAND_POINTS; i++) {
			bool tip = i == 4 || i == 8 || i == 12 || i == 16 || i == 20;

			raster_disc(r, (int)((ox + p->x[i]) * RQ_ONE), (int)((oy + p->y[i]) * RQ_ONE),
				    (tip ? 5 : 3) * RQ_ONE, tip ? C_AMBER : C_TEXT, 255);
		}
	}
}

static void view_draw(lv_event_t *e)
{
	struct raster r;
	bool fresh;

	if (!raster_begin(&r, e)) {
		return;
	}
	const uint16_t *f = camera_acquire(&fresh);

	if (f == NULL) {
		raster_fill(&r, r.box.x1, r.box.y1, r.box.x2, r.box.y2, C_BG1);
		camera_release();
		return;
	}
	/* only the part inside this redraw's clip area */
	int x0 = MAX(r.box.x1, r.clip.x1), x1 = MIN(r.box.x2, r.clip.x2);
	int y0 = MAX(r.box.y1, r.clip.y1), y1 = MIN(r.box.y2, r.clip.y2);

	if (x0 <= x1 && y0 <= y1) {
		uint32_t us;

		dma2d_rgb565(f + (y0 - r.box.y1) * CAM_W + (x0 - r.box.x1), CAM_W, x1 - x0 + 1, y1 - y0 + 1,
			     r.base + (size_t)y0 * r.stride + x0, r.stride, &us);
		copy_us = us;
	}
	camera_release();
	overlay(&r);
}

/* The picture fills the view: nothing needs painting under it. */
static void view_cover(lv_event_t *e)
{
	lv_cover_check_info_t *info = lv_event_get_param(e);

	if (info->res != LV_COVER_RES_MASKED) {
		info->res = LV_COVER_RES_COVER;
	}
}

static lv_obj_t *field(lv_obj_t *root, int y, const char *cap, uint32_t color, const lv_font_t *font,
		       lv_obj_t **caption)
{
	lv_obj_t *l, *c = ui_caption(root, cap);

	lv_obj_set_pos(c, SIDE_X, y);
	l = ui_label(root, font, color, "-");
	lv_obj_set_pos(l, SIDE_X, y + 14);
	if (caption != NULL) {
		*caption = c;
	}
	return l;
}

static void create(lv_obj_t *root)
{
	view = ui_box(root, PX, PY, CAM_W, CAM_H);
	lv_obj_add_event_cb(view, view_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(view, view_cover, LV_EVENT_COVER_CHECK, NULL);

	l_state = ui_label(root, &font_textm_16, C_TEXT2, "Starting the camera");
	lv_obj_align_to(l_state, view, LV_ALIGN_CENTER, 0, 0);

	l_gesture[0] = field(root, 8, "HAND 1", hand_color[0], &font_head_28, &l_cap[0]);
	l_gesture[1] = field(root, 64, "HAND 2", hand_color[1], &font_head_28, &l_cap[1]);
	l_npu = field(root, 120, "NPU PALM / LANDMARKS", C_AZURE, &font_mono_14, NULL);
	l_track = field(root, 164, "TRACKING", C_TEXT, &font_mono_14, NULL);
	l_cam = field(root, 208, "CAMERA", C_TEXT, &font_mono_14, NULL);
	l_exp = field(root, 252, "GAIN, EXPOSURE", C_TEXT2, &font_mono_14, NULL);
	lv_obj_t *note = ui_label(root, &font_text_14, C_TEXT3,
				  "Google MediaPipe palm detector and hand landmarks (Apache-2.0) on the "
				  "NPU, up to two hands; gestures from the joint angles. Picture: DCMIPP "
				  "and ST's ISP.");

	lv_obj_set_width(note, GFX_W - SIDE_X - UI_PAD);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_pos(note, SIDE_X, 302);
}

static void show(void)
{
	memset(&hands, 0, sizeof(hands));
	camera_start(); /* asynchronous: the picture arrives in about 0.1 s */
	gfx_lend_back_buffer(true);
	hands_enable(true);
}

static void hide(void)
{
	hands_enable(false);
	gfx_lend_back_buffer(false);
	camera_stop();
}

static void ms_text(char *buf, size_t len, uint32_t us)
{
	if (us == 0) {
		snprintf(buf, len, "-");
	} else {
		snprintf(buf, len, "%u.%u", us / 1000, (us % 1000) / 100);
	}
}

/* A slot's caption: which hand by the network's handedness output, when it is sure. */
static void caption_text(char *buf, size_t len, int slot, const struct hand *p)
{
	float h = p->handedness;
	unsigned int sure = (unsigned int)((h > 0.5f ? h : 1.0f - h) * 100 + 0.5f);

	if (!p->valid || sure < 65) {
		snprintf(buf, len, "HAND %d", slot + 1);
	} else {
		snprintf(buf, len, "HAND %d, %s %u %%", slot + 1, h > 0.5f ? "RIGHT" : "LEFT", sure);
	}
}

static void label_update(lv_obj_t *l, const char *text)
{
	/* only when it changes: each change is an invalid area for LVGL */
	if (strcmp(lv_label_get_text(l), text) != 0) {
		lv_label_set_text(l, text);
	}
}

static void tick(uint32_t now_ms)
{
	static uint32_t text_t0, seen_frames;
	struct camera_status st;
	char buf[48], a[12], b[12];

	camera_get_status(&st);
	if (st.running && st.frames != seen_frames) {
		seen_frames = st.frames;
		frame_gap_ms = now_ms - last_frame_ms;
		last_frame_ms = now_ms;
		hands_get(&hands); /* the newest result goes with the newest picture */
		lv_obj_invalidate(view);
		if (hands.n > 0 && !shell_in_attract()) {
			shell_note_input(); /* someone is using the screen: no attract mode under their hand */
		}
		if (!lv_obj_has_flag(l_state, LV_OBJ_FLAG_HIDDEN)) {
			lv_obj_add_flag(l_state, LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (now_ms - text_t0 < 250) {
		return;
	}
	text_t0 = now_ms;
	if (st.error && !st.running) {
		lv_label_set_text(l_state, st.error);
		lv_obj_remove_flag(l_state, LV_OBJ_FLAG_HIDDEN);
	}
	/* spread over two ticks: at most four labels change in one frame */
	static bool half;

	half = !half;
	if (half) {
		for (int h = 0; h < HANDS_MAX; h++) {
			const struct hand *p = &hands.hand[h];

			if (hands.error && h == 0) {
				label_update(l_gesture[h], "-");
			} else {
				label_update(l_gesture[h], p->valid ? hands_gesture_name(p->gesture, p->fingers)
								    : hands.n == 0 && h == 0 ? "searching" : "-");
			}
			caption_text(buf, sizeof(buf), h, p);
			label_update(l_cap[h], buf);
		}
	} else {
		ms_text(a, sizeof(a), hands.palm_us);
		ms_text(b, sizeof(b), hands.hand_us);
		snprintf(buf, sizeof(buf), "%s / %s ms", a, b);
		label_update(l_npu, hands.error ? hands.error : buf);
		if (hands.n == 0) {
			snprintf(buf, sizeof(buf), "no hand");
		} else {
			snprintf(buf, sizeof(buf), "%d hand%s, %u.%u fps", hands.n, hands.n > 1 ? "s" : "",
				 hands.fps_x10 / 10, hands.fps_x10 % 10);
		}
		label_update(l_track, buf);
		snprintf(buf, sizeof(buf), "%u.%u fps, copy %u.%u ms", st.fps_x10 / 10, st.fps_x10 % 10,
			 copy_us / 1000, (copy_us % 1000) / 100);
		label_update(l_cam, buf);
		snprintf(buf, sizeof(buf), "%u.%u dB, %u lines", st.gain_mdb / 1000, (st.gain_mdb % 1000) / 100,
			 st.exposure);
		label_update(l_exp, buf);
	}
}

static void mini(const struct raster *r)
{
	/* a lens: two rings */
	int cx = (r->box.x1 + r->box.x2) / 2, cy = (r->box.y1 + r->box.y2) / 2;

	raster_ring(r, cx * RQ_ONE, cy * RQ_ONE, 30 * RQ_ONE, 3 * RQ_ONE, C_LINE2, 255);
	raster_ring(r, cx * RQ_ONE, cy * RQ_ONE, 16 * RQ_ONE, 2 * RQ_ONE, C_AZURE, 255);
}

const struct screen screen_camera = {
	.title = "Camera",
	.blurb = "IMX335 live, hands on the NPU",
	.create = create,
	.show = show,
	.hide = hide,
	.tick = tick,
	.mini = mini,
	.attract_secs = 10,
};
