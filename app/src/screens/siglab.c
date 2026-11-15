/*
 * Signal Lab: triggered scope, log-frequency spectrum with peak hold and a
 * touch cursor, and a scrolling waterfall. One 1024-point FFT per frame.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>

#include <zephyr/kernel.h>

#include "dsp/sig.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define PX      16
#define SCOPE_Y 44
#define SCOPE_H 56
#define SPEC_Y  108
#define SPEC_H  146
#define WF_Y    262

static lv_obj_t *scope_pane, *spec_pane, *wf_pane;
static lv_obj_t *l_peak, *l_level, *l_fft, *l_hold, *l_cursor, *l_source;
static uint32_t lut[256];
static int cursor_col = -1;
static uint32_t cursor_until;
static int32_t ybuf[SIG_COLS], ybuf2[SIG_COLS];

/* Colour map for the waterfall: screen background through navy and azure to a
 * pale blue, with amber reserved for the top few dB.
 */
static void build_lut(void)
{
	static const struct {
		uint8_t pos;
		uint32_t rgb;
	} stops[] = {
		{0, C_BG0}, {70, 0x0E2444}, {125, 0x15539A}, {175, C_AZURE},
		{215, 0xC4E6FF}, {240, 0xFFE2A8}, {255, C_AMBER},
	};

	for (int i = 0; i < 256; i++) {
		int s = 0;

		while (s < (int)ARRAY_SIZE(stops) - 2 && i > stops[s + 1].pos) {
			s++;
		}
		int span = stops[s + 1].pos - stops[s].pos;
		int t = CLAMP((i - stops[s].pos) * 255 / span, 0, 255);

		lut[i] = lv_color_to_u32(ui_mix(stops[s].rgb, stops[s + 1].rgb, t)) | 0xff000000u;
	}
}

static inline int32_t db_to_y(const lv_area_t *box, float db)
{
	float t = CLAMP(db / SIG_DB_MIN, 0.0f, 1.0f);

	return (int32_t)((box->y1 + 1) * RQ_ONE + t * (lv_area_get_height(box) - 3) * RQ_ONE);
}

static void scope_draw(lv_event_t *e)
{
	const struct sig_view *v = sig_view();
	struct raster r;

	if (!raster_begin(&r, e)) {
		return;
	}
	int mid = (r.box.y1 + r.box.y2) / 2;
	int amp = (lv_area_get_height(&r.box) / 2 - 3) * RQ_ONE;

	/* auto-range to the window's peak, smoothed so the trace does not pump */
	static float gain = 1.0f;
	int peak = 1;

	for (int i = 0; i < SIG_COLS; i++) {
		peak = MAX(peak, LV_ABS(v->scope[i]));
	}
	gain += 0.08f * (MIN(30000.0f / peak, 16.0f) - gain);

	raster_fill(&r, r.box.x1, mid, r.box.x2, mid, C_LINE);
	for (int i = 0; i < SIG_COLS; i++) {
		ybuf[i] = mid * RQ_ONE + RQ_ONE / 2 - (int32_t)(v->scope[i] * gain * amp) / 32768;
	}
	raster_trace(&r, r.box.x1, ybuf, SIG_COLS, C_MINT, 11, 255);
}

static void spec_draw(lv_event_t *e)
{
	static const float grid_hz[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
	const struct sig_view *v = sig_view();
	struct raster r;

	if (!raster_begin(&r, e)) {
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(grid_hz); i++) {
		int x = r.box.x1 + sig_hz_col(grid_hz[i]);
		bool decade = grid_hz[i] == 100 || grid_hz[i] == 1000 || grid_hz[i] == 10000;

		raster_fill(&r, x, r.box.y1, x, r.box.y2, decade ? C_LINE2 : C_LINE);
	}
	for (int db = -20; db > -100; db -= 20) {
		raster_hline_dash(&r, r.box.x1, r.box.x2, db_to_y(&r.box, db) >> RQ, C_LINE, 255, 2, 4);
	}
	raster_fill(&r, r.box.x1, r.box.y2, r.box.x2, r.box.y2, C_LINE2);

	for (int i = 0; i < SIG_COLS; i++) {
		ybuf[i] = db_to_y(&r.box, v->col_db[i]);
		ybuf2[i] = db_to_y(&r.box, v->col_peak[i]);
	}
	raster_area(&r, r.box.x1, ybuf, SIG_COLS, r.box.y1, r.box.y2 - 1, C_AZURE, 110);
	raster_trace(&r, r.box.x1, ybuf2, SIG_COLS, C_AMBER, 8, 150);
	raster_trace(&r, r.box.x1, ybuf, SIG_COLS, C_AZURE, 12, 255);

	if (cursor_col >= 0) {
		int x = r.box.x1 + cursor_col;

		raster_fill_a(&r, x, r.box.y1, x, r.box.y2, C_TEXT, 150);
		raster_disc(&r, x * RQ_ONE + RQ_ONE / 2, ybuf[cursor_col], 4 * RQ_ONE, C_TEXT, 255);
	}
}

static void wf_draw(lv_event_t *e)
{
	const struct sig_view *v = sig_view();
	struct raster r;

	if (!raster_begin(&r, e) || v->waterfall == NULL) {
		return;
	}
	/* newest row on top: ring rows head..H-1 first, then 0..head-1 */
	int first = SIG_WF_H - v->wf_head;

	raster_lut8(&r, r.box.x1, r.box.y1, SIG_COLS, first, v->waterfall + v->wf_head * SIG_COLS,
		    SIG_COLS, lut);
	if (v->wf_head > 0) {
		raster_lut8(&r, r.box.x1, r.box.y1 + first, SIG_COLS, v->wf_head, v->waterfall, SIG_COLS,
			    lut);
	}
	if (cursor_col >= 0) {
		raster_fill_a(&r, r.box.x1 + cursor_col, r.box.y1, r.box.x1 + cursor_col, r.box.y2, C_TEXT,
			      90);
	}
}

static void spec_touch(lv_event_t *e)
{
	lv_point_t p;
	lv_area_t a;

	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(lv_event_get_current_target(e), &a);
	cursor_col = CLAMP(p.x - a.x1, 0, SIG_COLS - 1);
	cursor_until = k_uptime_get_32() + 4000;
}

static void wf_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	sig_hold(!sig_held());
	lv_label_set_text(l_hold, sig_held() ? "HOLD" : "RUN");
	lv_obj_set_style_text_color(l_hold, ui_c(sig_held() ? C_AMBER : C_MINT), 0);
}

static void source_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	sig_set_source(sig_source() == SIG_SRC_MIC ? SIG_SRC_GENERATOR : SIG_SRC_MIC);
}

static lv_obj_t *pane(lv_obj_t *root, int y, int h, lv_event_cb_t draw)
{
	lv_obj_t *p = ui_box(root, PX, y, SIG_COLS, h);

	lv_obj_add_event_cb(p, draw, LV_EVENT_DRAW_MAIN, NULL);
	return p;
}

static void create(lv_obj_t *root)
{
	build_lut();

	lv_obj_set_pos(ui_caption(root, "SOURCE  TAP TO SWITCH"), PX, 8);
	l_source = ui_label(root, &font_mono_14, C_TEXT, sig_view()->source);
	lv_obj_set_pos(l_source, PX, 22);
	/* the two lines above are the button */
	lv_obj_t *src_btn = ui_box(root, 0, 0, 212, 42);

	lv_obj_add_flag(src_btn, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(src_btn, source_click, LV_EVENT_SHORT_CLICKED, NULL);

	lv_obj_set_pos(ui_caption(root, "PEAK"), 220, 8);
	l_peak = ui_label(root, &font_mono_14, C_AZURE, "-");
	lv_obj_set_pos(l_peak, 220, 22);

	lv_obj_set_pos(ui_caption(root, "LEVEL"), 360, 8);
	l_level = ui_label(root, &font_mono_14, C_TEXT, "-");
	lv_obj_set_pos(l_level, 360, 22);

	lv_obj_set_pos(ui_caption(root, "1024-PT FFT"), 500, 8);
	l_fft = ui_label(root, &font_mono_14, C_TEXT, "-");
	lv_obj_set_pos(l_fft, 500, 22);

	l_hold = ui_label(root, &font_mono_14, C_MINT, "RUN");
	lv_obj_align(l_hold, LV_ALIGN_TOP_RIGHT, -PX, 22);
	lv_obj_t *cap = ui_caption(root, "TAP WATERFALL");

	lv_obj_align(cap, LV_ALIGN_TOP_RIGHT, -PX, 8);

	scope_pane = pane(root, SCOPE_Y, SCOPE_H, scope_draw);
	spec_pane = pane(root, SPEC_Y, SPEC_H, spec_draw);
	wf_pane = pane(root, WF_Y, SIG_WF_H, wf_draw);

	lv_obj_add_flag(spec_pane, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(spec_pane, spec_touch, LV_EVENT_PRESSING, NULL);
	lv_obj_add_flag(wf_pane, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(wf_pane, wf_click, LV_EVENT_SHORT_CLICKED, NULL);

	/* axis annotations, inside the spectrum pane */
	static const struct {
		float hz;
		const char *txt;
	} ticks[] = {{100, "100"}, {1000, "1k"}, {10000, "10k"}};

	for (size_t i = 0; i < ARRAY_SIZE(ticks); i++) {
		lv_obj_t *l = ui_caption(root, ticks[i].txt);

		lv_obj_set_pos(l, PX + sig_hz_col(ticks[i].hz) + 4, SPEC_Y + SPEC_H - 16);
	}
	for (int db = -20; db > -100; db -= 20) {
		lv_obj_t *l = ui_caption(root, "");

		lv_label_set_text_fmt(l, "%d dB", db);
		lv_obj_set_pos(l, PX + 4, SPEC_Y + 1 + (-db) * (SPEC_H - 3) / 100 - 14);
	}
	lv_obj_set_pos(ui_caption(root, "SCOPE  16 MS  AUTO-RANGED"), PX + 4, SCOPE_Y + 2);

	l_cursor = ui_label(root, &font_mono_12, C_TEXT, "");
	lv_obj_set_style_bg_color(l_cursor, ui_c(C_BG2), 0);
	lv_obj_set_style_bg_opa(l_cursor, LV_OPA_90, 0);
	lv_obj_set_style_pad_hor(l_cursor, 6, 0);
	lv_obj_set_style_pad_ver(l_cursor, 3, 0);
	lv_obj_set_style_radius(l_cursor, 4, 0);
	lv_obj_add_flag(l_cursor, LV_OBJ_FLAG_HIDDEN);
}

static void fmt_hz(char *buf, size_t n, float hz)
{
	if (hz >= 1000.0f) {
		snprintf(buf, n, "%.2f kHz", (double)(hz / 1000.0f));
	} else {
		snprintf(buf, n, "%.0f Hz", (double)hz);
	}
}

static void tick(uint32_t now_ms)
{
	static uint32_t last_text;
	const struct sig_view *v = sig_view();
	char buf[40], hz[16];

	lv_obj_invalidate(scope_pane);
	lv_obj_invalidate(spec_pane);
	lv_obj_invalidate(wf_pane);

	if (cursor_col >= 0 && (int32_t)(now_ms - cursor_until) > 0) {
		cursor_col = -1;
		lv_obj_add_flag(l_cursor, LV_OBJ_FLAG_HIDDEN);
	}
	if (cursor_col >= 0) {
		fmt_hz(hz, sizeof(hz), sig_col_hz(cursor_col));
		snprintf(buf, sizeof(buf), "%s  %.1f dB", hz, (double)v->col_db[cursor_col]);
		lv_label_set_text(l_cursor, buf);
		lv_obj_remove_flag(l_cursor, LV_OBJ_FLAG_HIDDEN);
		lv_obj_update_layout(l_cursor);
		int w = lv_obj_get_width(l_cursor);
		int x = PX + cursor_col + 8;

		if (x + w > GFX_W - PX) {
			x = PX + cursor_col - 8 - w;
		}
		lv_obj_set_pos(l_cursor, x, SPEC_Y + 6);
	}
	if (now_ms - last_text < 125) {
		return;
	}
	last_text = now_ms;
	if (lv_label_get_text(l_source) != v->source) {
		lv_label_set_text_static(l_source, v->source);
		lv_obj_set_style_text_color(l_source, ui_c(sig_source() == SIG_SRC_MIC ? C_MINT : C_TEXT), 0);
	}
	fmt_hz(hz, sizeof(hz), v->peak_hz);
	lv_label_set_text(l_peak, hz);
	snprintf(buf, sizeof(buf), "%.1f dBFS", (double)v->rms_db);
	lv_label_set_text(l_level, buf);
	snprintf(buf, sizeof(buf), "%u us", v->fft_us);
	lv_label_set_text(l_fft, buf);
}

static void hide(void)
{
	if (sig_held()) {
		wf_click(NULL);
	}
}

static void service(uint32_t now_ms)
{
	sig_service(now_ms);
}

static void mini(const struct raster *r)
{
	const struct sig_view *v = sig_view();
	static int32_t y[160];
	int w = MIN(lv_area_get_width(&r->box), (int)ARRAY_SIZE(y));
	int h = lv_area_get_height(&r->box);

	for (int i = 0; i < w; i++) {
		int c0 = i * SIG_COLS / w, c1 = (i + 1) * SIG_COLS / w;
		float m = SIG_DB_MIN;

		for (int c = c0; c < c1; c++) {
			m = MAX(m, v->col_db[c]);
		}
		y[i] = (r->box.y1 + 1) * RQ_ONE + (int32_t)(CLAMP(m / SIG_DB_MIN, 0.0f, 1.0f) * (h - 3) * RQ_ONE);
	}
	raster_area(r, r->box.x1, y, w, r->box.y1, r->box.y2, C_AZURE, 90);
	raster_trace(r, r->box.x1, y, w, C_AZURE, 11, 255);
}

const struct screen screen_siglab = {
	.title = "Signal Lab",
	.blurb = "Scope and spectrum",
	.create = create,
	.tick = tick,
	.hide = hide,
	.service = service,
	.mini = mini,
};
