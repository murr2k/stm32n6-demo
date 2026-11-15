/*
 * Flight Deck: the board's own telemetry. Render time per frame, CPU load
 * per thread, heaps and Ethernet throughput. All of it measured live.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "platform/net.h"
#include "platform/tele.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define ROWS       6
#define FRAME_BARS 172  /* 2 px each */
#define SPARK_W    346

static lv_obj_t *l_render, *l_refresh, *l_worst, *l_drop, *l_scale;
static lv_obj_t *frame_plot;
static lv_obj_t *cpu_arc, *l_cpu, *cpu_spark;
static lv_obj_t *l_heap, *l_ui, *bar_heap, *bar_ui, *l_uptime;
static lv_obj_t *thr_name[ROWS], *thr_pct[ROWS], *thr_bars;
static lv_obj_t *l_rx, *l_rx_unit, *l_tx, *l_tx_unit, *l_ip, *l_net_scale, *net_plot;
static uint32_t frame_scale_us = 2000;
static uint32_t last_sample;

/* ---- frame time plot ---------------------------------------------------- */

static void frame_plot_draw(lv_event_t *e)
{
	const struct gfx_stats *gs = gfx_stats();
	struct raster r;

	if (!raster_begin(&r, e)) {
		return;
	}
	int h = lv_area_get_height(&r.box);
	uint32_t peak = 0;

	for (int i = 0; i < FRAME_BARS; i++) {
		uint32_t v = gs->render_us[(gs->head + GFX_HISTORY - FRAME_BARS + i) % GFX_HISTORY];

		peak = MAX(peak, v);
	}
	/* full scale steps through 2, 4, 8 ms, then one and two refresh periods */
	static const uint32_t steps[] = {2000, 4000, 8000, 0, 0};
	uint32_t fs = gs->period_us * 2;

	for (size_t i = 0; i < ARRAY_SIZE(steps); i++) {
		uint32_t s = steps[i] ? steps[i] : (i == 3 ? gs->period_us : gs->period_us * 2);

		if (peak <= s) {
			fs = s;
			break;
		}
	}
	frame_scale_us = fs;

	raster_hline_dash(&r, r.box.x1, r.box.x2, r.box.y1, C_LINE2, 255, 3, 3);
	raster_hline_dash(&r, r.box.x1, r.box.x2, r.box.y1 + h / 2, C_LINE, 255, 3, 3);
	raster_fill(&r, r.box.x1, r.box.y2, r.box.x2, r.box.y2, C_LINE2);

	for (int i = 0; i < FRAME_BARS; i++) {
		uint32_t v = gs->render_us[(gs->head + GFX_HISTORY - FRAME_BARS + i) % GFX_HISTORY];
		int bh = (int)MIN((uint64_t)v * (h - 1) / fs, (uint64_t)(h - 1));
		uint32_t c = v > gs->period_us ? C_BAD : (v > gs->period_us * 3 / 4 ? C_WARN : C_AZURE);

		raster_fill(&r, r.box.x1 + i * 2, r.box.y2 - 1 - bh, r.box.x1 + i * 2, r.box.y2 - 1, c);
	}
}

/* ---- sparklines --------------------------------------------------------- */

static void cpu_spark_draw(lv_event_t *e)
{
	static int32_t y[152];
	const struct tele *t = tele_get();
	struct raster r;

	if (!raster_begin(&r, e)) {
		return;
	}
	int w = MIN(lv_area_get_width(&r.box), (int)ARRAY_SIZE(y));
	int h = lv_area_get_height(&r.box);

	for (int i = 0; i < w; i++) {
		/* newest sample at the right edge, one sample per 2 px */
		int age = (w - 1 - i) / 2;
		uint32_t v = t->cpu_ring[(t->head + TELE_HISTORY - 1 - age) % TELE_HISTORY];

		y[i] = ((r.box.y2 - 1) << RQ) - (int32_t)(v * ((h - 3) << RQ) / 1000);
	}
	raster_area(&r, r.box.x1, y, w, r.box.y1, r.box.y2, C_AZURE, 70);
	raster_trace(&r, r.box.x1, y, w, C_AZURE, 12, 255);
}

static void net_plot_draw(lv_event_t *e)
{
	static int32_t y_rx[SPARK_W], y_tx[SPARK_W];
	const struct tele *t = tele_get();
	struct raster r;

	if (!raster_begin(&r, e)) {
		return;
	}
	int w = MIN(lv_area_get_width(&r.box), SPARK_W);
	int h = lv_area_get_height(&r.box);
	uint32_t peak = 1024;

	for (int i = 0; i < TELE_HISTORY; i++) {
		peak = MAX(peak, MAX(t->rx_ring[i], t->tx_ring[i]));
	}
	/* round the scale up to 1, 2 or 5 times a power of ten */
	uint32_t decade = 1000;

	while (decade * 10 <= peak) {
		decade *= 10;
	}
	uint32_t fs = peak <= decade ? decade : peak <= 2 * decade ? 2 * decade
		      : peak <= 5 * decade ? 5 * decade : 10 * decade;

	lv_obj_set_user_data(lv_event_get_current_target(e), (void *)(uintptr_t)fs);

	for (int i = 0; i < w; i++) {
		int age = (w - 1 - i) / 2;
		int idx = (t->head + TELE_HISTORY - 1 - age) % TELE_HISTORY;
		int32_t full = (h - 3) << RQ;

		y_rx[i] = ((r.box.y2 - 1) << RQ) - (int32_t)((uint64_t)MIN(t->rx_ring[idx], fs) * full / fs);
		y_tx[i] = ((r.box.y2 - 1) << RQ) - (int32_t)((uint64_t)MIN(t->tx_ring[idx], fs) * full / fs);
	}
	raster_hline_dash(&r, r.box.x1, r.box.x2, r.box.y1, C_LINE2, 255, 3, 3);
	raster_fill(&r, r.box.x1, r.box.y2, r.box.x2, r.box.y2, C_LINE2);
	raster_area(&r, r.box.x1, y_rx, w, r.box.y1, r.box.y2, C_AZURE, 60);
	raster_trace(&r, r.box.x1, y_tx, w, C_AMBER, 12, 255);
	raster_trace(&r, r.box.x1, y_rx, w, C_AZURE, 12, 255);
}

/* ---- thread bars -------------------------------------------------------- */

static void thr_bars_draw(lv_event_t *e)
{
	const struct tele *t = tele_get();
	struct raster r;

	if (!raster_begin(&r, e)) {
		return;
	}
	int w = lv_area_get_width(&r.box);
	uint32_t fs = 100; /* 10 % minimum full scale */

	for (int i = 0; i < t->n_thr; i++) {
		fs = MAX(fs, t->thr[i].pct_x10);
	}
	for (int i = 0; i < ROWS; i++) {
		int y = r.box.y1 + i * 24 + 9;

		raster_fill(&r, r.box.x1, y, r.box.x2, y + 5, C_BG2);
		if (i < t->n_thr) {
			int bw = (int)(t->thr[i].pct_x10 * (uint32_t)w / fs);

			raster_fill(&r, r.box.x1, y, r.box.x1 + MAX(bw, 2) - 1, y + 5,
				    i == 0 ? C_AZURE : 0x3A8ACC);
		}
	}
}

/* ---- helpers ------------------------------------------------------------ */

static void meter(lv_obj_t *card, int y, const char *name, lv_obj_t **value, lv_obj_t **bar)
{
	lv_obj_t *cap = ui_caption(card, name);

	lv_obj_set_pos(cap, 16, y);
	*value = ui_label(card, &font_mono_14, C_TEXT, "-");
	lv_obj_set_pos(*value, 16, y + 16);

	lv_obj_t *track = ui_box(card, 16, y + 38, 152, 6);

	lv_obj_set_style_bg_color(track, ui_c(C_BG2), 0);
	lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(track, 3, 0);
	*bar = ui_box(track, 0, 0, 2, 6);
	lv_obj_set_style_bg_color(*bar, ui_c(C_AZURE), 0);
	lv_obj_set_style_bg_opa(*bar, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(*bar, 3, 0);
}

static void fmt_rate(uint32_t bps, char *val, size_t n, const char **unit)
{
	if (bps >= 1000000) {
		snprintf(val, n, "%u.%u", bps / 1000000, (bps / 100000) % 10);
		*unit = "MB/s";
	} else if (bps >= 1000) {
		snprintf(val, n, "%u.%u", bps / 1000, (bps / 100) % 10);
		*unit = "kB/s";
	} else {
		snprintf(val, n, "%u", bps);
		*unit = "B/s";
	}
}

static void arc_anim(void *obj, int32_t v)
{
	lv_arc_set_value(obj, v);
}

/* ---- build -------------------------------------------------------------- */

static void create(lv_obj_t *root)
{
	/* frame time */
	lv_obj_t *c = ui_card(root, 16, 16, 376, 200);

	lv_obj_set_pos(ui_caption(c, "RENDER TIME"), 16, 14);
	l_render = ui_label(c, &font_num_56, C_TEXT, "0.0");
	lv_obj_set_pos(l_render, 14, 30);
	lv_obj_t *ms = ui_label(c, &font_head_20, C_TEXT2, "ms");

	lv_obj_set_pos(ms, 124, 64);

	static const char *const caps[] = {"REFRESH", "WORST", "DROPPED"};
	lv_obj_t **vals[] = {&l_refresh, &l_worst, &l_drop};

	for (int i = 0; i < 3; i++) {
		lv_obj_set_pos(ui_caption(c, caps[i]), 196, 16 + i * 26);
		*vals[i] = ui_label(c, &font_mono_14, C_TEXT, "-");
		lv_obj_set_pos(*vals[i], 268, 14 + i * 26);
	}
	frame_plot = ui_box(c, 16, 106, FRAME_BARS * 2, 78);
	lv_obj_add_event_cb(frame_plot, frame_plot_draw, LV_EVENT_DRAW_MAIN, NULL);
	l_scale = ui_caption(c, "");
	lv_obj_set_pos(l_scale, 16, 90);

	/* cpu */
	c = ui_card(root, 404, 16, 184, 200);
	lv_obj_set_pos(ui_caption(c, "CPU LOAD"), 16, 14);
	cpu_arc = lv_arc_create(c);
	lv_obj_remove_style_all(cpu_arc);
	lv_obj_set_size(cpu_arc, 116, 116);
	lv_obj_set_pos(cpu_arc, 34, 36);
	lv_arc_set_rotation(cpu_arc, 135);
	lv_arc_set_bg_angles(cpu_arc, 0, 270);
	lv_arc_set_range(cpu_arc, 0, 1000);
	lv_arc_set_value(cpu_arc, 0);
	lv_obj_set_style_arc_width(cpu_arc, 9, LV_PART_MAIN);
	lv_obj_set_style_arc_color(cpu_arc, ui_c(C_BG2), LV_PART_MAIN);
	lv_obj_set_style_arc_rounded(cpu_arc, true, LV_PART_MAIN);
	lv_obj_set_style_arc_opa(cpu_arc, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_arc_width(cpu_arc, 9, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(cpu_arc, ui_c(C_AZURE), LV_PART_INDICATOR);
	lv_obj_set_style_arc_rounded(cpu_arc, true, LV_PART_INDICATOR);
	lv_obj_set_style_arc_opa(cpu_arc, LV_OPA_COVER, LV_PART_INDICATOR);
	lv_obj_remove_flag(cpu_arc, LV_OBJ_FLAG_CLICKABLE);
	l_cpu = ui_label(c, &font_num_32, C_TEXT, "0%");
	lv_obj_align_to(l_cpu, cpu_arc, LV_ALIGN_CENTER, 0, 0);
	cpu_spark = ui_box(c, 16, 154, 152, 30);
	lv_obj_add_event_cb(cpu_spark, cpu_spark_draw, LV_EVENT_DRAW_MAIN, NULL);

	/* memory */
	c = ui_card(root, 600, 16, 184, 200);
	meter(c, 14, "KERNEL HEAP", &l_heap, &bar_heap);
	meter(c, 72, "UI HEAP", &l_ui, &bar_ui);
	lv_obj_set_pos(ui_caption(c, "UPTIME"), 16, 134);
	l_uptime = ui_label(c, &font_mono_14, C_TEXT, "-");
	lv_obj_set_pos(l_uptime, 16, 150);
	lv_obj_set_pos(ui_caption(c, "800 MHz  M55"), 16, 174);

	/* threads */
	c = ui_card(root, 16, 228, 378, 200);
	lv_obj_set_pos(ui_caption(c, "THREADS  SHARE OF CPU TIME"), 16, 14);
	for (int i = 0; i < ROWS; i++) {
		thr_name[i] = ui_label(c, &font_mono_12, C_TEXT2, "");
		lv_obj_set_pos(thr_name[i], 16, 42 + i * 24);
		thr_pct[i] = ui_label(c, &font_mono_12, C_TEXT, "");
		lv_obj_set_width(thr_pct[i], 52);
		lv_obj_set_style_text_align(thr_pct[i], LV_TEXT_ALIGN_RIGHT, 0);
		lv_obj_set_pos(thr_pct[i], 310, 42 + i * 24);
	}
	thr_bars = ui_box(c, 132, 38, 172, ROWS * 24);
	lv_obj_add_event_cb(thr_bars, thr_bars_draw, LV_EVENT_DRAW_MAIN, NULL);

	/* network */
	c = ui_card(root, 406, 228, 378, 200);
	lv_obj_set_pos(ui_caption(c, "ETHERNET"), 16, 14);
	l_ip = ui_label(c, &font_mono_12, C_TEXT2, "");
	lv_obj_set_width(l_ip, 200);
	lv_obj_set_style_text_align(l_ip, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(l_ip, 162, 14);

	lv_obj_set_pos(ui_caption(c, "RX"), 16, 44);
	l_rx = ui_label(c, &font_num_32, C_AZURE, "0");
	lv_obj_set_pos(l_rx, 44, 34);
	l_rx_unit = ui_label(c, &font_text_14, C_TEXT2, "B/s");
	lv_obj_set_pos(ui_caption(c, "TX"), 196, 44);
	l_tx = ui_label(c, &font_num_32, C_AMBER, "0");
	lv_obj_set_pos(l_tx, 224, 34);
	l_tx_unit = ui_label(c, &font_text_14, C_TEXT2, "B/s");

	net_plot = ui_box(c, 16, 102, SPARK_W, 82);
	lv_obj_add_event_cb(net_plot, net_plot_draw, LV_EVENT_DRAW_MAIN, NULL);
	l_net_scale = ui_caption(c, "");
	lv_obj_set_pos(l_net_scale, 16, 86);
}

static void set_bar(lv_obj_t *bar, uint32_t used, uint32_t size)
{
	lv_obj_set_width(bar, size ? MAX((int32_t)((uint64_t)used * 152 / size), 2) : 2);
}

/* Labels only re-layout and repaint when their text really changed. */
static void set_txt(lv_obj_t *l, const char *fmt, ...)
{
	char buf[48];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (strcmp(lv_label_get_text(l), buf) != 0) {
		lv_label_set_text(l, buf);
	}
}

/* A new telemetry sample touches some thirty labels. Doing that in one frame
 * invalidates most of the screen at once; spreading it over five frames keeps
 * every frame a small partial redraw.
 */
static void update_stage(int stage, uint32_t now_ms)
{
	const struct gfx_stats *gs = gfx_stats();
	const struct tele *t = tele_get();
	const char *unit;
	char buf[32];

	switch (stage) {
	case 0:
		set_txt(l_render, "%u.%02u", gs->avg_render_us / 1000, (gs->avg_render_us / 10) % 100);
		set_txt(l_refresh, "%u.%02u Hz", gs->refresh_uhz / 1000000, (gs->refresh_uhz / 10000) % 100);
		set_txt(l_worst, "%u.%u ms", gs->worst_render_us / 1000, (gs->worst_render_us / 100) % 10);
		set_txt(l_drop, "%u", gs->dropped);
		lv_obj_set_style_text_color(l_drop, ui_c(gs->dropped ? C_WARN : C_TEXT), 0);
		set_txt(l_scale, "LAST %d FRAMES  FULL SCALE %u.%u MS", FRAME_BARS, frame_scale_us / 1000,
			(frame_scale_us / 100) % 10);
		break;
	case 1: {
		lv_anim_t a;

		lv_anim_init(&a);
		lv_anim_set_var(&a, cpu_arc);
		lv_anim_set_exec_cb(&a, arc_anim);
		lv_anim_set_values(&a, lv_arc_get_value(cpu_arc), t->cpu_pct_x10);
		lv_anim_set_duration(&a, TELE_PERIOD_MS);
		lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
		lv_anim_start(&a);
		set_txt(l_cpu, "%u%%", (t->cpu_pct_x10 + 5) / 10);
		lv_obj_align_to(l_cpu, cpu_arc, LV_ALIGN_CENTER, 0, 0);
		lv_obj_invalidate(cpu_spark);
		break;
	}
	case 2: {
		uint32_t s = now_ms / 1000;

		set_txt(l_heap, "%u.%u / %u KB", t->heap_used / 1024, (t->heap_used % 1024) * 10 / 1024,
			t->heap_size / 1024);
		set_bar(bar_heap, t->heap_used, t->heap_size);
		set_txt(l_ui, "%u.%u / %u KB", t->ui_used / 1024, (t->ui_used % 1024) * 10 / 1024,
			t->ui_size / 1024);
		set_bar(bar_ui, t->ui_used, t->ui_size);
		set_txt(l_uptime, "%ud %02u:%02u:%02u", s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
		break;
	}
	case 3:
		for (int i = 0; i < ROWS; i++) {
			if (i < t->n_thr) {
				set_txt(thr_name[i], "%s", t->thr[i].name);
				set_txt(thr_pct[i], "%u.%u%%", t->thr[i].pct_x10 / 10, t->thr[i].pct_x10 % 10);
			} else {
				set_txt(thr_name[i], "");
				set_txt(thr_pct[i], "");
			}
		}
		lv_obj_invalidate(thr_bars);
		break;
	case 4: {
		struct net_status ns;

		net_get_status(&ns);
		set_txt(l_ip, "%s", ns.have_ip ? ns.ip : (ns.link_up ? "waiting for DHCP" : "cable unplugged"));
		fmt_rate(t->rx_bps, buf, sizeof(buf), &unit);
		set_txt(l_rx, "%s", buf);
		set_txt(l_rx_unit, "%s", unit);
		lv_obj_align_to(l_rx_unit, l_rx, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -6);
		fmt_rate(t->tx_bps, buf, sizeof(buf), &unit);
		set_txt(l_tx, "%s", buf);
		set_txt(l_tx_unit, "%s", unit);
		lv_obj_align_to(l_tx_unit, l_tx, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -6);

		uint32_t fs = (uint32_t)(uintptr_t)lv_obj_get_user_data(net_plot);

		fmt_rate(fs ? fs : 1000, buf, sizeof(buf), &unit);
		set_txt(l_net_scale, "LAST %d S  FULL SCALE %s %s", SPARK_W / 2 * TELE_PERIOD_MS / 1000, buf,
			unit);
		lv_obj_invalidate(net_plot);
		break;
	}
	default:
		break;
	}
}

static void tick(uint32_t now_ms)
{
	static int stage = 5;
	const struct tele *t = tele_get();

	/* the frame plot scrolls every frame: this screen always renders at refresh */
	lv_obj_invalidate(frame_plot);

	if (stage >= 5 && t->samples != last_sample) {
		last_sample = t->samples;
		stage = 0;
	}
	if (stage < 5) {
		update_stage(stage++, now_ms);
	}
}

static void mini(const struct raster *r)
{
	const struct gfx_stats *gs = gfx_stats();
	int n = MIN(lv_area_get_width(&r->box) / 3, GFX_HISTORY);
	int h = lv_area_get_height(&r->box);
	uint32_t fs = 2000;

	for (int i = 0; i < n; i++) {
		fs = MAX(fs, (uint32_t)gs->render_us[(gs->head + GFX_HISTORY - n + i) % GFX_HISTORY]);
	}
	fs = fs * 5 / 4; /* headroom so the bars never touch the top */
	for (int i = 0; i < n; i++) {
		uint32_t v = gs->render_us[(gs->head + GFX_HISTORY - n + i) % GFX_HISTORY];
		int bh = (int)((uint64_t)v * (h - 1) / fs);

		raster_fill(r, r->box.x1 + i * 3, r->box.y2 - MAX(bh, 1), r->box.x1 + i * 3 + 1, r->box.y2,
			    v > gs->period_us ? C_BAD : C_AZURE);
	}
}

const struct screen screen_deck = {
	.title = "Flight Deck",
	.blurb = "The board, measured",
	.create = create,
	.tick = tick,
	.mini = mini,
};