/*
 * Camera: the IMX335 live, demosaiced and scaled by the DCMIPP, exposure and
 * colour by ST's ISP, copied to the display by DMA2D. The CPU only hands
 * buffers around. Beside the picture, what it costs and what the ISP chose.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "media/camera.h"
#include "media/jpegdec.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define PX     0
#define PY     0
#define SIDE_X (CAM_W + 16)

static lv_obj_t *view, *l_fps, *l_frame, *l_copy, *l_gain, *l_exp, *l_state;
static uint32_t copy_us, last_frame_ms, frame_gap_ms;

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
}

static lv_obj_t *field(lv_obj_t *root, int y, const char *cap, uint32_t color)
{
	lv_obj_t *l;

	lv_obj_set_pos(ui_caption(root, cap), SIDE_X, y);
	l = ui_label(root, &font_mono_14, color, "-");
	lv_obj_set_pos(l, SIDE_X, y + 14);
	return l;
}

static void create(lv_obj_t *root)
{
	view = ui_box(root, PX, PY, CAM_W, CAM_H);
	lv_obj_add_event_cb(view, view_draw, LV_EVENT_DRAW_MAIN, NULL);

	l_state = ui_label(root, &font_textm_16, C_TEXT2, "Starting the camera");
	lv_obj_align_to(l_state, view, LV_ALIGN_CENTER, 0, 0);

	l_fps = field(root, 8, "FRAMES PER SECOND", C_TEXT);
	l_frame = field(root, 52, "SENSOR", C_TEXT);
	lv_label_set_text(l_frame, "IMX335 2592x1944");
	lv_obj_t *l_pipe = field(root, 96, "PIPE", C_TEXT);

	lv_label_set_text_fmt(l_pipe, "DCMIPP > %dx%d", CAM_W, CAM_H);
	l_gain = field(root, 140, "ANALOGUE GAIN", C_AZURE);
	l_exp = field(root, 184, "EXPOSURE", C_AZURE);
	l_copy = field(root, 228, "DMA2D COPY", C_TEXT);
	lv_obj_t *note = ui_label(root, &font_text_14, C_TEXT3,
				  "Demosaic and scaling in the DCMIPP, exposure and white balance by "
				  "ST's ISP middleware, the copy to the display by DMA2D.");

	lv_obj_set_width(note, GFX_W - SIDE_X - UI_PAD);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_pos(note, SIDE_X, 290);
}

static void show(void)
{
	camera_start(); /* asynchronous: the picture arrives in about 0.1 s */
}

static void hide(void)
{
	camera_stop();
}

static void tick(uint32_t now_ms)
{
	static uint32_t text_t0, seen_frames;
	struct camera_status st;
	char buf[48];

	camera_get_status(&st);
	if (st.running && st.frames != seen_frames) {
		seen_frames = st.frames;
		frame_gap_ms = now_ms - last_frame_ms;
		last_frame_ms = now_ms;
		lv_obj_invalidate(view);
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
	snprintf(buf, sizeof(buf), "%u.%u", st.fps_x10 / 10, st.fps_x10 % 10);
	lv_label_set_text(l_fps, buf);
	snprintf(buf, sizeof(buf), "%u.%u dB", st.gain_mdb / 1000, (st.gain_mdb % 1000) / 100);
	lv_label_set_text(l_gain, buf);
	snprintf(buf, sizeof(buf), "%u lines", st.exposure);
	lv_label_set_text(l_exp, buf);
	snprintf(buf, sizeof(buf), "%u us", copy_us);
	lv_label_set_text(l_copy, buf);
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
	.blurb = "IMX335 live, ISP",
	.create = create,
	.show = show,
	.hide = hide,
	.tick = tick,
	.mini = mini,
	.attract_secs = 10,
};
