/*
 * Explorer: a million samples on four channels, panned and zoomed at full
 * frame rate. Each pixel column reads a few nodes of a min/max pyramid, so
 * the cost per frame is the same whether the view spans 105 seconds or 30 ms.
 *
 * Gestures use the raw multi-touch table: one finger pans (with inertia), two
 * fingers pinch around their midpoint, a tap places the cursor, a double tap
 * resets the view. Untouched, the screen tours its own data.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>

#include <zephyr/kernel.h>

#include "dsp/dataset.h"
#include "platform/touch.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define PX       16
#define PW       768
#define PLOT_Y   46          /* root-relative */
#define LANE_H   80
#define PLOT_H   (LANE_H * DS_CH)
#define OV_Y     390
#define OV_H     38
#define AXIS_H   18          /* time axis row under the lanes, part of the plot widget */
#define SPC_MAX  ((double)DS_N / PW)
#define SPC_MIN  0.04

static const uint32_t ch_color[DS_CH] = {C_AZURE, C_AMBER, C_VIOLET, C_MINT};

static lv_obj_t *plot, *overview, *l_wait;
static lv_obj_t *l_span, *l_samples, *l_perpx, *l_lod, *l_draw;

/* view: sample index at the left edge, samples per pixel column */
static double v_t0, v_spc = SPC_MAX;
static int cursor_x = -1;
static int lod_used;
static uint32_t draw_us;

/* gesture state */
static int g_prev_n;
static float g_last_x, g_last_d, g_last_mid, g_vel, g_moved;
static uint32_t g_down_ms, g_last_tap_ms, g_last_ms, g_touch_ms;
static bool g_in_overview;

/* animated view change (double tap, tour) */
static bool a_on;
static double a_t0_from, a_t0_to, a_spc_from, a_spc_to;
static uint32_t a_start, a_dur;
static int tour_step;
static uint32_t tour_next;

static void clamp_view(void)
{
	v_spc = CLAMP(v_spc, SPC_MIN, SPC_MAX);
	v_t0 = CLAMP(v_t0, 0.0, (double)DS_N - v_spc * PW);
}

static void view_to(double t0, double spc, uint32_t dur_ms)
{
	a_t0_from = v_t0;
	a_spc_from = v_spc;
	a_spc_to = CLAMP(spc, SPC_MIN, SPC_MAX);
	a_t0_to = CLAMP(t0, 0.0, (double)DS_N - a_spc_to * PW);
	a_start = k_uptime_get_32();
	a_dur = dur_ms;
	a_on = true;
}

/* ---- drawing ------------------------------------------------------------ */

static void plot_draw(lv_event_t *e)
{
	static int32_t lo_q[PW], hi_q[PW];
	struct raster r;
	uint32_t t_start = gfx_now_us();

	if (!raster_begin(&r, e)) {
		return;
	}
	if (!ds_ready()) {
		return;
	}
	/* time grid: a 1-2-5 step that gives 5 to 10 lines */
	double span_s = v_spc * PW / DS_FS;
	double step = pow(10.0, floor(log10(span_s / 5.0)));

	if (span_s / step > 25.0) {
		step *= 5.0;
	} else if (span_s / step > 10.0) {
		step *= 2.0;
	}
	double t_left = v_t0 / DS_FS;

	for (double t = ceil(t_left / step) * step; t < t_left + span_s; t += step) {
		int x = r.box.x1 + (int)((t - t_left) / span_s * PW);

		raster_fill(&r, x, r.box.y1, x, r.box.y1 + PLOT_H - 1, C_LINE);
	}

	lod_used = -1;
	for (int ch = 0; ch < DS_CH; ch++) {
		int y0 = r.box.y1 + ch * LANE_H;
		int mid_q = (y0 + LANE_H / 2) * RQ_ONE;
		int amp_q = (LANE_H / 2 - 5) * RQ_ONE;

		if (ch > 0) {
			raster_fill(&r, r.box.x1, y0, r.box.x2, y0, C_LINE2);
		}
		raster_hline_dash(&r, r.box.x1, r.box.x2, y0 + LANE_H / 2, C_LINE, 255, 2, 6);

		if (v_spc >= 1.0) {
			/* envelope: every column covers one or more samples */
			for (int x = 0; x < PW; x++) {
				uint32_t s0 = (uint32_t)(v_t0 + x * v_spc);
				uint32_t s1 = (uint32_t)(v_t0 + (x + 1) * v_spc);
				int16_t lo, hi;
				int level = ds_range(ch, s0, MAX(s1, s0 + 1), &lo, &hi);

				lod_used = MAX(lod_used, level);
				/* screen y grows downwards: the sample maximum is the top */
				lo_q[x] = mid_q - (hi * amp_q) / 32768;
				hi_q[x] = mid_q - (lo * amp_q) / 32768;
			}
			/* translucent body with crisp edges: a dense signal reads as a
			 * band with structure instead of a solid block
			 */
			raster_band(&r, r.box.x1, lo_q, hi_q, PW, ch_color[ch], 4, 85, 9);
		} else {
			/* zoomed past one sample per pixel: interpolate, then mark the samples */
			for (int x = 0; x < PW; x++) {
				double pos = v_t0 + (x + 0.5) * v_spc;
				uint32_t i = (uint32_t)pos;
				float f = (float)(pos - i);
				float v = ds_sample(ch, i) * (1.0f - f) + ds_sample(ch, i + 1) * f;

				lo_q[x] = mid_q - (int32_t)(v * amp_q) / 32768;
			}
			raster_trace(&r, r.box.x1, lo_q, PW, ch_color[ch], 12, 255);
			if (v_spc < 0.2) {
				for (uint32_t i = (uint32_t)v_t0; i < (uint32_t)(v_t0 + PW * v_spc) + 1; i++) {
					int xq = (int)((r.box.x1 + (i - v_t0) / v_spc) * RQ_ONE);

					raster_disc(&r, xq, mid_q - (ds_sample(ch, i) * amp_q) / 32768,
						    3 * RQ_ONE, ch_color[ch], 255);
				}
			}
		}
	}
	int y_axis = r.box.y1 + PLOT_H;
	lv_layer_t *layer = lv_event_get_layer(e);
	char buf[24];

	raster_fill(&r, r.box.x1, y_axis - 1, r.box.x2, y_axis - 1, C_LINE2);

	/* time labels, one per grid line */
	int dec = step >= 1.0 ? 0 : step >= 0.1 ? 1 : step >= 0.01 ? 2 : step >= 0.001 ? 3 : 4;

	for (double t = ceil(t_left / step) * step; t < t_left + span_s; t += step) {
		int x = r.box.x1 + (int)((t - t_left) / span_s * PW);

		if (x > r.box.x2 - 60) {
			break;
		}
		snprintf(buf, sizeof(buf), "%.*f s", dec, t);
		ui_draw_text(layer, &font_mono_12, C_TEXT3, x + 4, y_axis + 3, buf);
	}

	if (cursor_x >= 0) {
		int cx = r.box.x1 + cursor_x;
		uint32_t s = (uint32_t)(v_t0 + (cursor_x + 0.5) * v_spc);

		raster_fill_a(&r, cx, r.box.y1, cx, y_axis - 2, C_TEXT, 170);
		for (int ch = 0; ch < DS_CH; ch++) {
			snprintf(buf, sizeof(buf), "%+.3f", (double)(ds_sample(ch, s) / 30000.0f));
			int w = ui_text_width(&font_mono_12, buf);
			int x = cursor_x > PW - 80 ? cx - 16 - w : cx + 8;
			int y = r.box.y1 + ch * LANE_H + LANE_H - 24;

			raster_fill_a(&r, x - 5, y - 2, x + w + 4, y + 15, C_BG2, 230);
			ui_draw_text(layer, &font_mono_12, ch_color[ch], x, y, buf);
		}
	}
	draw_us = gfx_now_us() - t_start;
}

static void overview_draw_into(const struct raster *r, const lv_area_t *box, int ch, uint32_t rgb,
			       uint8_t alpha)
{
	static int32_t lo_q[PW], hi_q[PW];
	int count;
	const struct ds_node *top = ds_top(ch, &count);
	int w = MIN(lv_area_get_width(box), PW);
	int mid_q = ((box->y1 + box->y2) / 2) * RQ_ONE;
	int amp_q = (lv_area_get_height(box) / 2 - 2) * RQ_ONE;

	for (int x = 0; x < w; x++) {
		int k0 = x * count / w, k1 = MAX((x + 1) * count / w, k0 + 1);
		int16_t lo = INT16_MAX, hi = INT16_MIN;

		for (int k = k0; k < k1; k++) {
			lo = MIN(lo, top[k].lo);
			hi = MAX(hi, top[k].hi);
		}
		lo_q[x] = mid_q - (hi * amp_q) / 32768;
		hi_q[x] = mid_q - (lo * amp_q) / 32768;
	}
	raster_envelope(r, box->x1, lo_q, hi_q, w, rgb, 8, alpha);
}

static void overview_draw(lv_event_t *e)
{
	struct raster r;

	if (!raster_begin(&r, e) || !ds_ready()) {
		return;
	}
	raster_fill(&r, r.box.x1, r.box.y1, r.box.x2, r.box.y2, C_BG1);
	overview_draw_into(&r, &r.box, 0, C_TEXT3, 255);

	int x1 = r.box.x1 + (int)(v_t0 / DS_N * PW);
	int x2 = r.box.x1 + (int)((v_t0 + v_spc * PW) / DS_N * PW);

	x2 = MAX(x2, x1 + 2);
	raster_fill_a(&r, x1, r.box.y1, x2, r.box.y2, C_AZURE, 60);
	raster_fill(&r, x1, r.box.y1, x1, r.box.y2, C_AZURE);
	raster_fill(&r, x2, r.box.y1, x2, r.box.y2, C_AZURE);
}

/* ---- build -------------------------------------------------------------- */

static void create(lv_obj_t *root)
{
	static const char *const caps[] = {"SPAN", "SAMPLES IN VIEW", "PER PIXEL", "PYRAMID", "PLOT DRAW"};
	static const int cap_x[] = {PX, PX + 120, PX + 280, PX + 410, PX + 530};
	lv_obj_t **vals[] = {&l_span, &l_samples, &l_perpx, &l_lod, &l_draw};

	for (int i = 0; i < 5; i++) {
		lv_obj_set_pos(ui_caption(root, caps[i]), cap_x[i], 8);
		*vals[i] = ui_label(root, &font_mono_14, C_TEXT, "-");
		lv_obj_set_pos(*vals[i], cap_x[i], 22);
	}
	lv_obj_t *hint = ui_caption(root, "SYNTHETIC DATA");

	lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -PX, 8);
	hint = ui_label(root, &font_mono_14, C_TEXT2, "2^20 x 4 ch");
	lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -PX, 22);

	plot = ui_box(root, PX, PLOT_Y, PW, PLOT_H + AXIS_H);
	lv_obj_add_event_cb(plot, plot_draw, LV_EVENT_DRAW_MAIN, NULL);
	overview = ui_box(root, PX, OV_Y, PW, OV_H);
	lv_obj_add_event_cb(overview, overview_draw, LV_EVENT_DRAW_MAIN, NULL);

	for (int ch = 0; ch < DS_CH; ch++) {
		lv_obj_t *l = ui_label(root, &font_mono_12, ch_color[ch], ds_name(ch));

		lv_obj_set_style_text_letter_space(l, 1, 0);
		lv_obj_set_style_bg_color(l, ui_c(C_BG0), 0);
		lv_obj_set_style_bg_opa(l, LV_OPA_80, 0);
		lv_obj_set_style_pad_hor(l, 5, 0);
		lv_obj_set_style_pad_ver(l, 2, 0);
		lv_obj_set_style_radius(l, 4, 0);
		lv_obj_set_pos(l, PX + 4, PLOT_Y + ch * LANE_H + 4);

	}

	l_wait = ui_label(root, &font_textm_20, C_TEXT2, "");
	lv_obj_align(l_wait, LV_ALIGN_CENTER, 0, 0);
}

/* ---- gestures ----------------------------------------------------------- */

static void gestures(uint32_t now_ms)
{
	struct touch_state ts;
	float x[2] = {0};
	float dt = (now_ms - g_last_ms) * 0.001f;
	int n = 0, y0 = 0;

	g_last_ms = now_ms;
	touch_get(&ts);
	for (int s = 0; s < TOUCH_MAX_POINTS && n < 2; s++) {
		if (ts.pt[s].down && ts.pt[s].y >= UI_CONTENT_Y + PLOT_Y - 6) {
			if (n == 0) {
				y0 = ts.pt[s].y;
			}
			x[n++] = ts.pt[s].x;
		}
	}
	if (shell_gesture_active()) {
		n = 0;
	}
	if (n > 0) {
		g_touch_ms = now_ms;
		a_on = false;
	}

	if (n == 1) {
		if (g_prev_n != 1) {
			g_last_x = x[0];
			g_down_ms = now_ms;
			g_moved = 0;
			g_vel = 0;
			g_in_overview = y0 >= UI_CONTENT_Y + OV_Y - 8;
		}
		if (g_in_overview) {
			/* the overview is a scrubber: centre the view under the finger */
			v_t0 = (double)(x[0] - PX) / PW * DS_N - v_spc * PW / 2;
		} else {
			float dx = x[0] - g_last_x;

			v_t0 -= dx * v_spc;
			g_moved += fabsf(dx);
			if (dt > 0.0f) {
				g_vel = 0.6f * g_vel + 0.4f * (dx / dt);
			}
		}
		g_last_x = x[0];
	} else if (n == 2) {
		float d = MAX(fabsf(x[1] - x[0]), 24.0f);
		float mid = 0.5f * (x[0] + x[1]);

		if (g_prev_n == 2) {
			/* keep the sample under the midpoint fixed while the scale changes */
			double focus = v_t0 + (g_last_mid - PX) * v_spc;

			v_spc = CLAMP(v_spc * (double)(g_last_d / d), SPC_MIN, SPC_MAX);
			v_t0 = focus - (mid - PX) * v_spc;
		}
		g_last_d = d;
		g_last_mid = mid;
		g_moved = 100;
		g_vel = 0;
	} else {
		if (g_prev_n == 1 && !g_in_overview && g_moved < 10 && now_ms - g_down_ms < 260) {
			if (now_ms - g_last_tap_ms < 320) {
				view_to(0, SPC_MAX, 420);
				cursor_x = -1;
				g_last_tap_ms = 0;
			} else {
				cursor_x = CLAMP((int)g_last_x - PX, 0, PW - 1);
				g_last_tap_ms = now_ms;
			}
			g_vel = 0;
		}
		/* inertia: velocity decays with a 0.33 s time constant */
		if (fabsf(g_vel) > 20.0f) {
			v_t0 -= g_vel * dt * v_spc;
			g_vel *= expf(-dt / 0.33f);
		} else {
			g_vel = 0;
		}
	}
	g_prev_n = n;
	clamp_view();
}

/* Untouched for a while: walk through the zoom levels so the pyramid shows. */
static void tour(uint32_t now_ms)
{
	static const struct tour_stop {
		double centre; /* fraction of the dataset */
		double spc;
		uint16_t move_ms, hold_ms;
	} stops[] = {
		{0.50, SPC_MAX, 1200, 2200}, {0.37, 260.0, 1800, 2200}, {0.37, 26.0, 1800, 2000},
		{0.372, 2.0, 1800, 2200},    {0.372, 0.12, 1600, 2600}, {0.62, 40.0, 2200, 2200},
	};

	if (now_ms - g_touch_ms < 7000 || a_on || (int32_t)(now_ms - tour_next) < 0) {
		return;
	}
	const struct tour_stop *s = &stops[tour_step];

	view_to(s->centre * DS_N - s->spc * PW / 2, s->spc, s->move_ms);
	tour_next = now_ms + s->move_ms + s->hold_ms;
	tour_step = (tour_step + 1) % ARRAY_SIZE(stops);
	cursor_x = -1;
}

static void animate(uint32_t now_ms)
{
	if (!a_on) {
		return;
	}
	float t = MIN((float)(now_ms - a_start) / a_dur, 1.0f);
	float e = t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; /* ease in-out cubic */

	/* interpolate the scale geometrically and the centre linearly: a straight zoom */
	double c_from = a_t0_from + a_spc_from * PW / 2, c_to = a_t0_to + a_spc_to * PW / 2;

	v_spc = a_spc_from * pow(a_spc_to / a_spc_from, e);
	v_t0 = c_from + (c_to - c_from) * e - v_spc * PW / 2;
	clamp_view();
	if (t >= 1.0f) {
		a_on = false;
	}
}

static void tick(uint32_t now_ms)
{
	static uint32_t text_t0;
	char buf[40];

	if (!ds_ready()) {
		snprintf(buf, sizeof(buf), "Preparing dataset  %d %%", ds_progress_pct());
		lv_label_set_text(l_wait, buf);
		lv_obj_align(l_wait, LV_ALIGN_CENTER, 0, 0);
		return;
	}
	if (!lv_obj_has_flag(l_wait, LV_OBJ_FLAG_HIDDEN)) {
		lv_obj_add_flag(l_wait, LV_OBJ_FLAG_HIDDEN);
		g_touch_ms = now_ms - 5500; /* start the tour shortly after the data is ready */
	}

	double prev_t0 = v_t0, prev_spc = v_spc;
	int prev_cursor = cursor_x;

	gestures(now_ms);
	tour(now_ms);
	animate(now_ms);

	if (v_t0 != prev_t0 || v_spc != prev_spc || cursor_x != prev_cursor || text_t0 == 0) {
		lv_obj_invalidate(plot);
		lv_obj_invalidate(overview);
	}

	if (now_ms - text_t0 >= 100) {
		double span_s = v_spc * PW / DS_FS;
		double samples = v_spc * PW;

		text_t0 = now_ms;
		if (span_s >= 1.0) {
			snprintf(buf, sizeof(buf), "%.2f s", span_s);
		} else {
			snprintf(buf, sizeof(buf), "%.1f ms", span_s * 1000.0);
		}
		lv_label_set_text(l_span, buf);
		if (samples >= 10000.0) {
			snprintf(buf, sizeof(buf), "%.0f k", samples / 1000.0);
		} else {
			snprintf(buf, sizeof(buf), "%.0f", samples);
		}
		lv_label_set_text(l_samples, buf);
		snprintf(buf, sizeof(buf), v_spc >= 10.0 ? "%.0f" : "%.2f", v_spc);
		lv_label_set_text(l_perpx, buf);
		if (lod_used < 0) {
			lv_label_set_text(l_lod, "raw");
		} else {
			lv_label_set_text_fmt(l_lod, "L%d  x%u", lod_used, 4u << (2 * lod_used));
		}
		lv_label_set_text_fmt(l_draw, "%u us", draw_us);
	}
}

static void show(void)
{
	g_touch_ms = k_uptime_get_32() - 5000;
	g_prev_n = 0;
}

static void mini(const struct raster *r)
{
	if (!ds_ready()) {
		return;
	}
	overview_draw_into(r, &r->box, 0, C_AZURE, 255);
}

const struct screen screen_explorer = {
	.title = "Explorer",
	.blurb = "A million samples",
	.create = create,
	.tick = tick,
	.show = show,
	.mini = mini,
	.attract_secs = 24, /* long enough for its own tour of the zoom levels */
};