/*
 * Home Power: the household's electricity, from the user's energy monitor
 * over the internet (platform/homepower.c), laid out and coloured like the
 * power section of the monitor's own web page in its default skin, Notch:
 * near-black ground, zinc cards with hairline borders, a cyan trace, a
 * notched gauge whose thin arc is the last 24 hours' range.
 *
 * Its tile is on Home, and the screen is in the edge-swipe order, only while
 * the monitor answers. Everything here is redrawn when new figures arrive
 * (every 15 s while it shows), not every frame.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "platform/homepower.h"
#include "platform/psram.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

/* The Notch skin's colours (the monitor's page, html[data-skin="notch"]) */
#define N_BG     0x0A0A0A
#define N_CARD   0x111113 /* rgba(17, 17, 19, 0.92) */
#define N_BORDER 0x29292B /* rgba(255, 255, 255, 0.1) on the card */
#define N_TEXT   0xFAFAFA
#define N_MUTED  0x9F9FA9
#define N_CYAN   0x53EAFD /* the trace, "live", the lowest threshold */
#define N_CYAN2  0x00B8DB /* the 24-hour range on the gauge, the min-max band */
#define N_YELLOW 0xFBBF24
#define N_ORANGE 0xFB923C
#define N_RED    0xFB7185
#define N_STALE  0x52525C
#define N_STALE2 0x71717B

#define PI_F     3.14159265f
#define STALE_S  120 /* as the page: an older reading is not shown as the power now */

/* the gauge, in the page's own SVG coordinates (viewBox 280 x 180) */
#define G_W      280
#define G_H      210 /* and the live indicator under it */
#define G_CX     140
#define G_CY     140
#define G_R      110
#define G_TRACK  20
#define G_MAX_W  10000.0f
#define G_NOTCH  40

/* the chart's plot inside its box */
#define CH_W     424
#define CH_H     176
#define PL_X     56
#define PL_W     (CH_W - PL_X)
#define PL_Y     6
#define PL_H     150

static lv_obj_t *gauge, *chart, *range_btn[HP_RANGES], *range_txt[HP_RANGES];
static lv_obj_t *v24[4], *dstat[4], *dlab[4], *note, *bill[4], *bill_sub;

/* the chart's columns and scratch, in PSRAM: internal RAM is nearly full */
struct cols {
	int32_t avg[PL_W], lo[PL_W], hi[PL_W]; /* 1/16 px from the box's top */
	int32_t m[PL_W], top[PL_W], bot[PL_W], base[PL_W]; /* the same, on screen */
	int16_t cnt[PL_W];
	float sum[PL_W], mn[PL_W], mx[PL_W];
	float ncos[G_NOTCH], nsin[G_NOTCH]; /* the notches' middles, for the tile */
};
static struct cols *cl;
static int c_first, c_last; /* columns with data */
static float c_ymax;
static int c_step;
static uint8_t c_range;
static int64_t c_span_ms;

static struct hp_stats st;
static uint32_t t_gauge, t_chart, t_tick, t_mini; /* worst, us (ctl "powert") */
static uint32_t shown_seq = UINT32_MAX, shown_series = UINT32_MAX;
static bool shown_stale = true, shown_online, dirty = true, series_dirty = true;
static int reveal;
static char live_txt[24], chart_msg[64];
static int shown_range = -1;

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *o = ui_box(parent, x, y, w, h);

	lv_obj_set_style_bg_color(o, ui_c(N_CARD), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(o, ui_c(N_BORDER), 0);
	lv_obj_set_style_border_width(o, 1, 0);
	lv_obj_set_style_radius(o, 12, 0);
	return o;
}

static void fmt_w(char *b, size_t n, float w)
{
	if (fabsf(w) >= 1000.0f) {
		snprintf(b, n, "%.1f kW", (double)(w / 1000.0f));
	} else {
		snprintf(b, n, "%d W", (int)lroundf(w));
	}
}

static void fmt_energy(char *b, size_t n, float wh)
{
	if (wh >= 1e6f) {
		snprintf(b, n, "%.2f MWh", (double)(wh / 1e6f));
	} else if (wh >= 1e3f) {
		snprintf(b, n, "%.2f kWh", (double)(wh / 1e3f));
	} else {
		snprintf(b, n, "%.2f Wh", (double)wh);
	}
}

/* the page's stat thresholds: steps above the cyan base */
static uint32_t th(float v, float yellow, float upper, uint32_t upper_color)
{
	return v >= upper ? upper_color : v >= yellow ? N_YELLOW : N_CYAN;
}

static void set_text(lv_obj_t *l, const char *t)
{
	if (strcmp(lv_label_get_text(l), t) != 0) {
		lv_label_set_text(l, t);
	}
}

static void set_color(lv_obj_t *l, uint32_t c)
{
	lv_obj_set_style_text_color(l, ui_c(c), 0);
}

/* ---- gauge --------------------------------------------------------------- */

/* the point at angle phi along the arc (0 at the left end, pi at the right) */
static void arc_pt(const struct raster *r, float phi, float rad, float *x, float *y)
{
	*x = r->box.x1 + G_CX - rad * cosf(phi);
	*y = r->box.y1 + G_CY - rad * sinf(phi);
}

static float w_to_phi(float w)
{
	return CLAMP(w, 0.0f, G_MAX_W) / G_MAX_W * PI_F;
}

static int32_t text_top(const lv_font_t *f, int baseline)
{
	return baseline - (lv_font_get_line_height(f) - f->base_line);
}

static void text_mid(lv_layer_t *layer, const lv_font_t *f, uint32_t c, int cx, int baseline, const char *t)
{
	ui_draw_text(layer, f, c, cx - ui_text_width(f, t) / 2, text_top(f, baseline), t);
}

static void gauge_paint(lv_event_t *e)
{
	struct raster r;
	lv_layer_t *layer = lv_event_get_layer(e);
	bool stale = shown_stale;

	if (!raster_begin(&r, e)) {
		return;
	}
	/* the track, cut into 40 notches: dash 4, gap 4.64 along the 345.6 arc */
	for (int k = 0; k < G_NOTCH; k++) {
		float p0 = 8.64f * k / G_R, p1 = (8.64f * k + 4.0f) / G_R;
		float x[4], y[4];

		arc_pt(&r, p0, G_R - G_TRACK / 2, &x[0], &y[0]);
		arc_pt(&r, p0, G_R + G_TRACK / 2, &x[1], &y[1]);
		arc_pt(&r, p1, G_R + G_TRACK / 2, &x[2], &y[2]);
		arc_pt(&r, p1, G_R - G_TRACK / 2, &x[3], &y[3]);
		raster_poly(&r, x, y, 4, N_TEXT, 41); /* stroke-opacity 0.16 */
	}
	/* the typical range: the last 24 hours' minimum to maximum, 4 px */
	float lo = stale ? 1000.0f : (st.min_w > 0 ? st.min_w : 500.0f);
	float hi = stale ? 3000.0f : (st.max_w > 0 ? st.max_w : 3000.0f);
	float a0 = w_to_phi(lo), a1 = w_to_phi(hi);
	int steps = MAX(1, (int)((a1 - a0) / 0.08f)); /* chords 0.09 px off the arc */
	float px, py;

	arc_pt(&r, a0, G_R, &px, &py);
	for (int i = 1; i <= steps; i++) {
		float x, y;

		arc_pt(&r, a0 + (a1 - a0) * i / steps, G_R, &x, &y);
		raster_line(&r, (int)(px * RQ_ONE), (int)(py * RQ_ONE), (int)(x * RQ_ONE), (int)(y * RQ_ONE),
			    N_CYAN2, 2 * RQ_ONE, 255);
		px = x;
		py = y;
	}
	/* the needle: tip 90 px out, 10 px wide at the hub */
	float a = stale ? -90.0f : (CLAMP(st.now_w, 0.0f, G_MAX_W) / G_MAX_W * 180.0f - 90.0f);
	float s = sinf(a * PI_F / 180.0f), c = cosf(a * PI_F / 180.0f);
	static const float nx[4] = {0, -5, 0, 5}, ny[4] = {-90, 0, 5, 0};
	float x[4], y[4];

	for (int i = 0; i < 4; i++) {
		x[i] = r.box.x1 + G_CX + nx[i] * c - ny[i] * s;
		y[i] = r.box.y1 + G_CY + nx[i] * s + ny[i] * c;
	}
	raster_poly(&r, x, y, 4, stale ? N_STALE : 0xFFFFFF, 255);
	/* the hub */
	int hx = (r.box.x1 + G_CX) * RQ_ONE, hy = (r.box.y1 + G_CY) * RQ_ONE;

	raster_disc(&r, hx, hy, 13 * RQ_ONE, 0xFFFFFF, 77);
	raster_disc(&r, hx, hy, 11 * RQ_ONE, N_BG, 255);

	char b[16];

	if (stale) {
		strcpy(b, "--");
	} else {
		snprintf(b, sizeof(b), "%d", (int)lroundf(st.now_w));
	}
	text_mid(layer, &font_head_40, stale ? N_STALE : N_TEXT, r.box.x1 + G_CX, r.box.y1 + 125, b);
	text_mid(layer, &font_text_16, N_MUTED, r.box.x1 + G_CX, r.box.y1 + 145, "W");
	text_mid(layer, &font_mono_12, N_MUTED, r.box.x1 + 20, r.box.y1 + 158, "0");
	text_mid(layer, &font_mono_12, N_MUTED, r.box.x1 + 248, r.box.y1 + 158, "10k");

	/* the live indicator: a dot and LIVE, or how old the reading is */
	int tw = ui_text_width(&font_mono_12, live_txt);
	int x0 = r.box.x1 + G_CX - (tw + 14) / 2;
	int yb = r.box.y1 + 196;

	raster_disc(&r, (x0 + 4) * RQ_ONE, (yb - 4) * RQ_ONE, 4 * RQ_ONE, stale ? N_STALE : N_CYAN, 255);
	ui_draw_text(layer, &font_mono_12, stale ? N_STALE2 : N_CYAN, x0 + 14, text_top(&font_mono_12, yb),
		     live_txt);
}

static void gauge_draw(lv_event_t *e)
{
	uint32_t t0 = gfx_now_us();

	gauge_paint(e);
	t_gauge = MAX(t_gauge, gfx_now_us() - t0);
}

/* ---- chart --------------------------------------------------------------- */

static int nice_step(float ymax)
{
	static const int steps[] = {50, 100, 200, 250, 500, 1000, 2000, 2500, 5000, 10000, 20000};

	for (size_t i = 0; i < ARRAY_SIZE(steps); i++) {
		if (ymax / steps[i] <= 5.0f) {
			return steps[i];
		}
	}
	return 50000;
}

static int32_t y_of(float w)
{
	return ((PL_Y + PL_H) << RQ) - (int32_t)(w / c_ymax * (PL_H << RQ));
}

/* Each plot column takes the buckets in it (mean of means, lowest minimum,
 * highest maximum); columns between buckets are interpolated. */
static void build_columns(const struct hp_series *s)
{
	float top = 0;

	c_range = s->range;
	c_span_ms = MAX(s->end_ms - s->start_ms, 1);
	c_first = PL_W;
	c_last = -1;
	memset(cl->cnt, 0, sizeof(cl->cnt));
	for (int i = 0; i < s->n; i++) {
		int c = (int)((int64_t)s->t_s[i] * 1000 * (PL_W - 1) / c_span_ms);

		if (c < 0 || c >= PL_W) {
			continue;
		}
		if (cl->cnt[c] == 0) {
			cl->sum[c] = 0;
			cl->mn[c] = s->lo[i];
			cl->mx[c] = s->hi[i];
		}
		cl->cnt[c]++;
		cl->sum[c] += s->avg[i];
		cl->mn[c] = MIN(cl->mn[c], s->lo[i]);
		cl->mx[c] = MAX(cl->mx[c], s->hi[i]);
		top = MAX(top, s->hi[i]);
		c_first = MIN(c_first, c);
		c_last = MAX(c_last, c);
	}
	c_ymax = top > 0 ? top * 1.1f : 1000.0f;
	c_step = nice_step(c_ymax);
	int prev = -1;

	for (int c = c_first; c <= c_last; c++) {
		if (cl->cnt[c] == 0) {
			continue;
		}
		cl->avg[c] = y_of(cl->sum[c] / cl->cnt[c]);
		cl->lo[c] = y_of(cl->mx[c]); /* the top of the band on screen */
		cl->hi[c] = y_of(cl->mn[c]);
		if (prev >= 0 && c - prev > 1) {
			for (int k = prev + 1; k < c; k++) {
				int32_t f = (k - prev) * 256 / (c - prev);

				cl->avg[k] = cl->avg[prev] + (cl->avg[c] - cl->avg[prev]) * f / 256;
				cl->lo[k] = cl->lo[prev] + (cl->lo[c] - cl->lo[prev]) * f / 256;
				cl->hi[k] = cl->hi[prev] + (cl->hi[c] - cl->hi[prev]) * f / 256;
			}
		}
		prev = c;
	}
}

static void chart_paint(lv_event_t *e)
{
	struct raster r;
	lv_layer_t *layer = lv_event_get_layer(e);
	char b[24];

	if (!raster_begin(&r, e)) {
		return;
	}
	int x0 = r.box.x1 + PL_X, y0 = r.box.y1;

	if (chart_msg[0]) {
		text_mid(layer, &font_text_14, N_MUTED, r.box.x1 + CH_W / 2, y0 + CH_H / 2, chart_msg);
		return;
	}
	/* grid and the power axis */
	for (int v = 0; v <= (int)c_ymax; v += c_step) {
		int y = y0 + (y_of((float)v) >> RQ);

		raster_fill_a(&r, x0, y, x0 + PL_W - 1, y, 0xFFFFFF, v == 0 ? 31 : 18);
		fmt_w(b, sizeof(b), (float)v);
		ui_draw_text(layer, &font_mono_12, N_MUTED, x0 - 8 - ui_text_width(&font_mono_12, b), y - 8, b);
	}
	/* the time axis, back from the end of the window (the board keeps no clock
	 * of the house's time zone) */
	static const int32_t tick_s[HP_RANGES] = {900, 3600, 6 * 3600, 86400, 5 * 86400};
	static const char *const unit[HP_RANGES] = {"m", "h", "h", "d", "d"};
	static const int32_t unit_s[HP_RANGES] = {60, 3600, 3600, 86400, 86400};

	for (int64_t back = 0; back * 1000 <= c_span_ms; back += tick_s[c_range]) {
		int x = x0 + PL_W - 1 - (int)(back * 1000 * (PL_W - 1) / c_span_ms);

		raster_fill_a(&r, x, y0 + PL_Y + PL_H + 1, x, y0 + PL_Y + PL_H + 4, 0xFFFFFF, 31);
		if (back == 0) {
			strcpy(b, "now");
		} else {
			snprintf(b, sizeof(b), "-%d%s", (int)(back / unit_s[c_range]), unit[c_range]);
		}
		int w = ui_text_width(&font_mono_12, b);

		ui_draw_text(layer, &font_mono_12, N_MUTED, CLAMP(x - w / 2, x0, x0 + PL_W - w), y0 + PL_Y + PL_H + 5,
			     b);
	}
	if (c_last < c_first) {
		return;
	}
	int n = c_last - c_first + 1;

	/* the page's series: the mean with a faint fill under it, the band
	 * between each bucket's minimum and maximum, its edges at half strength */
	for (int i = 0; i < n; i++) {
		cl->m[i] = cl->avg[c_first + i] + (y0 << RQ);
		cl->top[i] = cl->lo[c_first + i] + (y0 << RQ);
		cl->bot[i] = cl->hi[c_first + i] + (y0 << RQ);
		cl->base[i] = (y0 + PL_Y + PL_H) << RQ;
	}
	raster_envelope(&r, x0 + c_first, cl->m, cl->base, n, N_CYAN, 0, 18);
	raster_envelope(&r, x0 + c_first, cl->top, cl->bot, n, N_CYAN2, 0, 36);
	raster_trace(&r, x0 + c_first, cl->top, n, N_CYAN2, 8, 128);
	raster_trace(&r, x0 + c_first, cl->bot, n, N_CYAN2, 8, 128);
	raster_trace(&r, x0 + c_first, cl->m, n, N_CYAN, 16, 255);
}

static void chart_draw(lv_event_t *e)
{
	uint32_t t0 = gfx_now_us();

	chart_paint(e);
	t_chart = MAX(t_chart, gfx_now_us() - t0);
}

/* ---- labels -------------------------------------------------------------- */

static void show_stats(bool stale)
{
	char b[32];
	const char *dash = "--";

	if (stale) {
		for (int i = 0; i < 4; i++) {
			set_text(v24[i], dash);
		}
	} else {
		snprintf(b, sizeof(b), "%dW", (int)lroundf(st.min_w));
		set_text(v24[0], b);
		snprintf(b, sizeof(b), "%dW", (int)lroundf(st.avg_w));
		set_text(v24[1], b);
		snprintf(b, sizeof(b), "%dW", (int)lroundf(st.max_w));
		set_text(v24[2], b);
		snprintf(b, sizeof(b), "$%.2f", (double)st.cost_24h);
		set_text(v24[3], b);
	}
	if (st.seq && st.have_bill) {
		snprintf(b, sizeof(b), "$%.2f", (double)st.bill_total);
		set_text(bill[0], b);
		if (st.have_trend) {
			snprintf(b, sizeof(b), "$%.2f/day", (double)st.slope_per_day);
			set_text(bill[1], b);
			snprintf(b, sizeof(b), "$%.2f", (double)st.projected);
			set_text(bill[2], b);
		} else {
			set_text(bill[1], dash);
			set_text(bill[2], dash);
		}
		snprintf(b, sizeof(b), "%d", MAX(0, st.cycle_days - st.days));
		set_text(bill[3], b);
		snprintf(b, sizeof(b), "day %d of %d \xC2\xB7 %d kWh", st.days, st.cycle_days, (int)lroundf(st.energy_kwh));
		set_text(bill_sub, b);
	} else if (st.seq == 0) {
		for (int i = 0; i < 4; i++) {
			set_text(bill[i], dash);
		}
		set_text(bill_sub, "");
	}
}

static void show_series(const struct hp_series *s)
{
	char b[96];

	build_columns(s);
	fmt_w(b, sizeof(b), s->min_w);
	set_text(dstat[0], b);
	set_color(dstat[0], th(s->min_w, 500, 1000, N_ORANGE));
	fmt_w(b, sizeof(b), s->mean_w);
	set_text(dstat[1], b);
	set_color(dstat[1], th(s->mean_w, 2000, 3000, N_ORANGE));
	fmt_w(b, sizeof(b), s->max_w);
	set_text(dstat[2], b);
	set_color(dstat[2], th(s->max_w, 3000, 5000, N_RED));
	fmt_energy(b, sizeof(b), s->energy_wh);
	set_text(dstat[3], b);
	set_color(dstat[3], th(s->energy_wh, 5000, 10000, N_ORANGE));
	snprintf(b, sizeof(b), "meter %d kWh \xC2\xB7 $%.4f/kWh \xC2\xB7 $%.2f/h \xC2\xB7 est. $%.2f over %s",
		 (int)lroundf(s->meter_kwh), (double)s->rate, (double)s->cost_hour, (double)s->est_cost,
		 homepower_range_name(s->range));
	set_text(note, b);
	if (s->n == 0) {
		strcpy(chart_msg, "No data in this range");
	} else {
		chart_msg[0] = '\0';
	}
	lv_obj_invalidate(chart);
}

static void style_ranges(int sel)
{
	for (int i = 0; i < HP_RANGES; i++) {
		bool on = i == sel;

		lv_obj_set_style_bg_opa(range_btn[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		set_color(range_txt[i], on ? N_BG : N_MUTED);
	}
}

static void range_click(lv_event_t *e)
{
	homepower_set_range((int)(intptr_t)lv_event_get_user_data(e));
}

/* ---- screen -------------------------------------------------------------- */

static lv_obj_t *stat_cell(lv_obj_t *parent, int x, int y, const char *label, lv_obj_t **value)
{
	*value = ui_label(parent, &font_textm_20, N_TEXT, "--");
	lv_obj_set_pos(*value, x, y);
	lv_obj_t *l = ui_label(parent, &font_text_14, N_MUTED, label);

	lv_obj_set_pos(l, x, y + 28);
	return l;
}

static void create(lv_obj_t *root)
{
	cl = psram_alloc(sizeof(*cl), 32);
	c_first = PL_W;
	c_last = -1;
	for (int k = 0; k < G_NOTCH; k++) {
		float phi = (8.64f * k + 2.0f) / G_R;

		cl->ncos[k] = cosf(phi);
		cl->nsin[k] = sinf(phi);
	}

	lv_obj_set_style_bg_color(root, ui_c(N_BG), 0);
	lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

	/* the power now */
	lv_obj_t *p = card(root, 16, 12, 296, 420);
	lv_obj_t *t = ui_label(p, &font_head_20, N_TEXT, "Real-Time Power Consumption");

	lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 15);
	t = ui_label(p, &font_mono_12, N_MUTED, "HOUSEHOLD METER, VIA THE INTERNET");
	lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 43);
	gauge = ui_box(p, (296 - 2 - G_W) / 2, 64, G_W, G_H);
	lv_obj_add_event_cb(gauge, gauge_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_t *rule = ui_box(p, 16, 290, 262, 1);

	lv_obj_set_style_bg_color(rule, ui_c(N_BORDER), 0);
	lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
	static const char *const l24[4] = {"24h Min", "24h Avg", "24h Max", "24h Cost"};

	for (int i = 0; i < 4; i++) {
		stat_cell(p, 24 + (i % 2) * 140, 306 + (i / 2) * 58, l24[i], &v24[i]);
	}

	/* trends */
	lv_obj_t *c = card(root, 328, 12, 456, 310);
	const char *title = "Power Consumption Trends";

	if (ui_text_width(&font_head_20, title) > 196) {
		title = "Consumption Trends";
	}
	lv_obj_set_pos(ui_label(c, &font_head_20, N_TEXT, title), 16, 14);
	lv_obj_t *picker = card(c, 454 - 16 - 214, 10, 214, 32);

	lv_obj_set_style_radius(picker, 8, 0);
	for (int i = 0; i < HP_RANGES; i++) {
		range_btn[i] = ui_box(picker, 3 + i * 42, 3, 40, 24);
		lv_obj_set_style_radius(range_btn[i], 6, 0);
		lv_obj_set_style_bg_color(range_btn[i], ui_c(N_TEXT), 0);
		lv_obj_add_flag(range_btn[i], LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(range_btn[i], 6);
		lv_obj_add_event_cb(range_btn[i], range_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		range_txt[i] = ui_label(range_btn[i], &font_textm_14, N_MUTED, homepower_range_name(i));
		lv_obj_center(range_txt[i]);
	}
	shown_range = homepower_range();
	style_ranges(shown_range);
	chart = ui_box(c, 16, 52, CH_W, CH_H);
	lv_obj_add_event_cb(chart, chart_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN); /* until a frame after show() */
	static const char *const ld[4] = {"Min Power", "Average Power", "Peak Power", "Energy Consumed"};

	for (int i = 0; i < 4; i++) {
		dlab[i] = stat_cell(c, 16 + i * 106, 236, ld[i], &dstat[i]);
	}
	note = ui_label(c, &font_mono_12, N_MUTED, "");
	lv_obj_set_pos(note, 16, 284);

	/* the bill */
	lv_obj_t *bc = card(root, 328, 334, 456, 98);

	lv_obj_set_pos(ui_label(bc, &font_head_20, N_TEXT, "Electricity Bill Forecast"), 16, 12);
	bill_sub = ui_label(bc, &font_mono_12, N_MUTED, "");
	lv_obj_align(bill_sub, LV_ALIGN_TOP_RIGHT, -16, 17);
	static const char *const lb[4] = {"Bill So Far", "Spend Rate", "Projected Bill", "Days Left"};

	for (int i = 0; i < 4; i++) {
		lv_obj_t *l = stat_cell(bc, 16 + i * 106, 40, lb[i], &bill[i]);

		lv_obj_set_y(l, 40 + 26);
	}
	strcpy(chart_msg, "Waiting for the monitor");
	strcpy(live_txt, "CONNECTING");
}

static void live_text(char *b, size_t n, uint32_t now)
{
	if (!homepower_configured()) {
		snprintf(b, n, "NOT SET UP");
		return;
	}
	if (st.seq == 0) {
		snprintf(b, n, "%s", homepower_online() ? "CONNECTING" : "OFFLINE");
		return;
	}
	int age = (st.age_s >= 0 ? st.age_s : 0) + (int)((now - st.fetched_ms) / 1000);

	if (age <= STALE_S) {
		snprintf(b, n, "LIVE");
	} else if (age >= 86400) {
		snprintf(b, n, "%dd %dh ago", age / 86400, age / 3600 % 24);
	} else if (age >= 3600) {
		snprintf(b, n, "%dh %dm ago", age / 3600, age / 60 % 60);
	} else {
		snprintf(b, n, "%dm ago", age / 60);
	}
}

static void tick_work(uint32_t now);

static void tick(uint32_t now)
{
	uint32_t t0 = gfx_now_us();

	tick_work(now);
	t_tick = MAX(t_tick, gfx_now_us() - t0);
}

void power_timing(uint32_t out[4], bool clear)
{
	out[0] = t_gauge;
	out[1] = t_chart;
	out[2] = t_tick;
	out[3] = t_mini;
	if (clear) {
		t_gauge = t_chart = t_tick = t_mini = 0;
	}
}

static void tick_work(uint32_t now)
{
	char lt[sizeof(live_txt)];

	homepower_get_stats(&st);
	live_text(lt, sizeof(lt), now);
	bool stale = strcmp(lt, "LIVE") != 0;
	bool online = homepower_online();

	if (dirty || st.seq != shown_seq || stale != shown_stale || strcmp(lt, live_txt) != 0) {
		shown_seq = st.seq;
		shown_stale = stale;
		strcpy(live_txt, lt);
		show_stats(stale);
		lv_obj_invalidate(gauge);
	}
	int sel = homepower_range();

	if (sel != shown_range) {
		shown_range = sel;
		style_ranges(sel);
	}
	dirty = false;
	/* The chart comes a frame after the rest: the first frame of the screen,
	 * drawn in the dark of the fade, had 1.9 ms to spare with it. */
	if (reveal > 0) {
		if (--reveal > 0) {
			return;
		}
		lv_obj_remove_flag(chart, LV_OBJ_FLAG_HIDDEN);
		series_dirty = true;
	}
	const struct hp_series *s = homepower_series();

	if (s && s->range == sel && (series_dirty || s->seq != shown_series)) {
		shown_series = s->seq;
		show_series(s);
	} else if ((s == NULL || s->range != sel) && (series_dirty || online != shown_online || shown_series != 0)) {
		/* nothing for this range yet: no figures from another range or time */
		shown_series = 0;
		snprintf(chart_msg, sizeof(chart_msg), "%s",
			 !homepower_configured() ? "No monitor address set (ctl.py \"power url ...\")"
			 : online		 ? "Loading"
						 : homepower_state());
		chart_msg[0] = (char)toupper((unsigned char)chart_msg[0]);
		for (int i = 0; i < 4; i++) {
			set_text(dstat[i], "--");
			set_color(dstat[i], N_TEXT);
		}
		set_text(note, "");
		lv_obj_invalidate(chart);
	}
	shown_online = online;
	series_dirty = false;
}

static void show(void)
{
	homepower_set_visible(true);
	dirty = true;
	reveal = 2;
}

static void hide(void)
{
	homepower_set_visible(false);
	lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);
}

/* ---- the Home tile: the panel's gauge at half size, live ----------------- */

/* Home redraws a visible tile every frame, so this gauge is strokes, not the
 * panel's polygons: its notches are short radial lines, the range a chain of
 * a dozen. */
#define M_R     56
#define M_TRACK 12

static bool mini_stale(const struct hp_stats *m, uint32_t now)
{
	int age = (m->age_s >= 0 ? m->age_s : 0) + (int)((now - m->fetched_ms) / 1000);

	return m->seq == 0 || age > STALE_S;
}

static void mini_centre(const struct raster *r, int *cx, int *cy)
{
	*cx = (r->box.x1 + r->box.x2 + 1) / 2;
	*cy = r->box.y2 - 21;
}

static int q(float v)
{
	return (int)(v * RQ_ONE);
}

static void mini(const struct raster *r)
{
	uint32_t t0 = gfx_now_us();
	struct hp_stats m;
	int cx, cy;

	homepower_get_stats(&m);
	bool stale = mini_stale(&m, k_uptime_get_32());

	mini_centre(r, &cx, &cy);
	for (int k = 0; k < G_NOTCH; k++) {
		float c = cl->ncos[k], s = cl->nsin[k];
		float r0 = M_R - M_TRACK / 2 + 1, r1 = M_R + M_TRACK / 2 - 1;

		raster_line(r, q(cx - r0 * c), q(cy - r0 * s), q(cx - r1 * c), q(cy - r1 * s), N_TEXT, 16, 41);
	}
	float lo = stale ? 1000.0f : (m.min_w > 0 ? m.min_w : 500.0f);
	float hi = stale ? 3000.0f : (m.max_w > 0 ? m.max_w : 3000.0f);
	float a0 = w_to_phi(lo), a1 = w_to_phi(hi);
	int steps = MAX(1, (int)((a1 - a0) / 0.12f));
	float px = cx - M_R * cosf(a0), py = cy - M_R * sinf(a0);

	for (int i = 1; i <= steps; i++) {
		float a = a0 + (a1 - a0) * i / steps;
		float x = cx - M_R * cosf(a), y = cy - M_R * sinf(a);

		raster_line(r, q(px), q(py), q(x), q(y), N_CYAN2, 24, 255);
		px = x;
		py = y;
	}
	float a = stale ? -90.0f : (CLAMP(m.now_w, 0.0f, G_MAX_W) / G_MAX_W * 180.0f - 90.0f);
	float s = sinf(a * PI_F / 180.0f), c = cosf(a * PI_F / 180.0f);
	static const float nx[4] = {0, -3, 0, 3}, ny[4] = {-46, 0, 3, 0};
	float x[4], y[4];

	for (int i = 0; i < 4; i++) {
		x[i] = cx + nx[i] * c - ny[i] * s;
		y[i] = cy + nx[i] * s + ny[i] * c;
	}
	raster_poly(r, x, y, 4, stale ? N_STALE : 0xFFFFFF, 255);
	raster_disc(r, cx * RQ_ONE, cy * RQ_ONE, 7 * RQ_ONE, 0xFFFFFF, 77);
	raster_disc(r, cx * RQ_ONE, cy * RQ_ONE, 6 * RQ_ONE, C_BG1, 255);
	t_mini = MAX(t_mini, gfx_now_us() - t0);
}

static void mini_text(lv_layer_t *layer, const struct raster *r)
{
	struct hp_stats m;
	int cx, cy;
	char b[12];

	homepower_get_stats(&m);
	bool stale = mini_stale(&m, k_uptime_get_32());

	mini_centre(r, &cx, &cy);
	if (stale) {
		strcpy(b, "--");
	} else {
		snprintf(b, sizeof(b), "%d", (int)lroundf(m.now_w));
	}
	text_mid(layer, &font_num_32, stale ? N_STALE : N_TEXT, cx, cy - 12, b);
	text_mid(layer, &font_text_14, N_MUTED, cx, cy + 5, "W");
	text_mid(layer, &font_mono_12, N_MUTED, cx - M_R, cy + 18, "0");
	text_mid(layer, &font_mono_12, N_MUTED, cx + M_R, cy + 18, "10k");
}

/* While Home shows, its tile's gauge is live: the figures every 15 s. */
static void service(uint32_t now)
{
	ARG_UNUSED(now);
	homepower_set_glance(shell_current() == 0);
}

const struct screen screen_power = {
	.title = "Home Power",
	.blurb = "The house's meter, online",
	.create = create,
	.tick = tick,
	.show = show,
	.hide = hide,
	.service = service,
	.mini = mini,
	.mini_text = mini_text,
	.available = homepower_online,
	.listed = homepower_online,
};
