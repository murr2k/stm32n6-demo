/*
 * Host Link: what a PC on the network is doing, pushed to the board ten
 * times a second by host/companion.py. Per-core load as a scrolling heat
 * map, totals, and the round-trip time of each datagram as the host measured
 * it. Without the companion the screen says so; it never invents data.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>

#include <zephyr/kernel.h>

#include "platform/hostlink.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define MAP_X 16
#define MAP_Y 62
#define MAP_W (HL_HISTORY * 2)
#define MAP_H 232
#define RTT_Y 326
#define RTT_H 100

static lv_obj_t *map, *rtt_plot, *l_host, *l_pps, *l_lost, *l_rtt, *l_cpu, *l_mem, *bar_mem;
static lv_obj_t *l_rx, *l_tx, *l_wait, *l_map_cap, *l_rtt_scale;
static uint32_t lut[101];
static uint32_t seen_packets;
static uint32_t rtt_scale_us = 1000;

static void build_lut(void)
{
	/* idle cores recede into the card, busy ones go azure, saturated ones amber */
	for (int i = 0; i <= 100; i++) {
		lv_color_t c = i < 70 ? ui_mix(C_BG2, C_AZURE, i * 255 / 70) : ui_mix(C_AZURE, C_AMBER, (i - 70) * 255 / 30);

		lut[i] = lv_color_to_u32(c) & 0xffffff;
	}
}

static void map_draw(lv_event_t *e)
{
	const struct hl_state *s = hostlink_state();
	struct raster r;

	if (!raster_begin(&r, e) || s->n_cores == 0) {
		return;
	}
	int rows = s->n_cores;
	int rh = MAP_H / rows;
	int gap = rh >= 6 ? 1 : 0;

	for (int c = 0; c < HL_HISTORY; c++) {
		const uint8_t *col = s->core_hist[(s->head + c) % HL_HISTORY]; /* oldest first */
		int x = r.box.x1 + c * 2;

		for (int k = 0; k < rows; k++) {
			raster_fill(&r, x, r.box.y1 + k * rh, x + 1, r.box.y1 + k * rh + rh - 1 - gap,
				    lut[MIN(col[k], 100)]);
		}
	}
}

static void rtt_draw(lv_event_t *e)
{
	static int32_t y[GFX_W];
	const struct hl_state *s = hostlink_state();
	struct raster r;

	if (!raster_begin(&r, e)) {
		return;
	}
	int w = MIN(lv_area_get_width(&r.box), (int)ARRAY_SIZE(y)), h = lv_area_get_height(&r.box);
	uint32_t peak = 200;

	for (int i = 0; i < HL_HISTORY; i++) {
		peak = MAX(peak, s->rtt_hist[i]);
	}
	uint32_t decade = 100;

	while (decade * 10 <= peak) {
		decade *= 10;
	}
	uint32_t fs = peak <= decade ? decade : peak <= 2 * decade ? 2 * decade : peak <= 5 * decade ? 5 * decade : 10 * decade;

	rtt_scale_us = fs;
	for (int i = 0; i < w; i++) {
		int idx = (s->head + (i * HL_HISTORY) / w) % HL_HISTORY;

		y[i] = ((r.box.y2 - 1) << RQ) - (int32_t)((uint64_t)MIN(s->rtt_hist[idx], fs) * ((h - 4) << RQ) / fs);
	}
	raster_hline_dash(&r, r.box.x1, r.box.x2, r.box.y1, C_LINE2, 255, 3, 3);
	raster_hline_dash(&r, r.box.x1, r.box.x2, r.box.y1 + h / 2, C_LINE, 255, 3, 3);
	raster_fill(&r, r.box.x1, r.box.y2, r.box.x2, r.box.y2, C_LINE2);
	raster_area(&r, r.box.x1, y, w, r.box.y1, r.box.y2, C_AZURE, 70);
	raster_trace(&r, r.box.x1, y, w, C_AZURE, 12, 255);
}

static lv_obj_t *field(lv_obj_t *root, int x, int y, const char *cap, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *l;

	lv_obj_set_pos(ui_caption(root, cap), x, y);
	l = ui_label(root, font, color, "-");
	lv_obj_set_pos(l, x, y + 14);
	return l;
}

static void create(lv_obj_t *root)
{
	build_lut();
	l_host = field(root, 16, 8, "HOST", &font_mono_14, C_TEXT);
	l_pps = field(root, 190, 8, "DATAGRAMS", &font_mono_14, C_TEXT);
	l_lost = field(root, 330, 8, "LOST", &font_mono_14, C_TEXT);
	l_rtt = field(root, 430, 8, "ROUND TRIP", &font_mono_14, C_AZURE);

	l_map_cap = ui_caption(root, "CPU CORES  LAST 25 S");
	lv_obj_set_pos(l_map_cap, MAP_X, MAP_Y - 16);
	map = ui_box(root, MAP_X, MAP_Y, MAP_W, MAP_H);
	lv_obj_add_event_cb(map, map_draw, LV_EVENT_DRAW_MAIN, NULL);

	lv_obj_t *c = ui_card(root, 532, 46, 252, 248);

	lv_obj_set_pos(ui_caption(c, "HOST CPU"), 16, 14);
	l_cpu = ui_label(c, &font_num_56, C_TEXT, "0%");
	lv_obj_set_pos(l_cpu, 14, 28);
	lv_obj_set_pos(ui_caption(c, "MEMORY IN USE"), 16, 104);
	l_mem = ui_label(c, &font_mono_14, C_TEXT, "-");
	lv_obj_set_pos(l_mem, 16, 120);
	lv_obj_t *track = ui_box(c, 16, 142, 220, 6);

	lv_obj_set_style_bg_color(track, ui_c(C_BG2), 0);
	lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(track, 3, 0);
	bar_mem = ui_box(track, 0, 0, 2, 6);
	lv_obj_set_style_bg_color(bar_mem, ui_c(C_AZURE), 0);
	lv_obj_set_style_bg_opa(bar_mem, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(bar_mem, 3, 0);
	lv_obj_set_pos(ui_caption(c, "HOST NETWORK"), 16, 164);
	l_rx = ui_label(c, &font_mono_14, C_AZURE, "-");
	lv_obj_set_pos(l_rx, 16, 182);
	l_tx = ui_label(c, &font_mono_14, C_AMBER, "-");
	lv_obj_set_pos(l_tx, 16, 204);

	l_rtt_scale = ui_caption(root, "");
	lv_obj_set_pos(l_rtt_scale, 16, RTT_Y - 16);
	rtt_plot = ui_box(root, 16, RTT_Y, 768, RTT_H);
	lv_obj_add_event_cb(rtt_plot, rtt_draw, LV_EVENT_DRAW_MAIN, NULL);

	l_wait = ui_label(root, &font_textm_20, C_TEXT2, "Waiting for the host companion");
	lv_obj_set_style_bg_color(l_wait, ui_c(C_BG0), 0);
	lv_obj_set_style_bg_opa(l_wait, LV_OPA_90, 0);
	lv_obj_set_style_pad_all(l_wait, 14, 0);
	lv_obj_set_style_radius(l_wait, 8, 0);
	lv_obj_align(l_wait, LV_ALIGN_CENTER, 0, -10);
	lv_obj_t *how = ui_label(root, &font_mono_12, C_TEXT3, "ON THE PC:  host\\run.cmd");

	lv_obj_align_to(how, l_wait, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
	lv_obj_set_user_data(l_wait, how);
}

static void fmt_rate(char *buf, size_t n, const char *tag, uint32_t bps)
{
	if (bps >= 1000000) {
		snprintf(buf, n, "%s %u.%u MB/s", tag, bps / 1000000, (bps / 100000) % 10);
	} else if (bps >= 1000) {
		snprintf(buf, n, "%s %u.%u kB/s", tag, bps / 1000, (bps / 100) % 10);
	} else {
		snprintf(buf, n, "%s %u B/s", tag, bps);
	}
}

static void tick(uint32_t now_ms)
{
	static uint32_t pps_t0, pps_n0, pps;
	static bool was_online = true;
	const struct hl_state *s = hostlink_state();
	bool online = hostlink_online();
	char buf[40];

	if (online != was_online) {
		lv_obj_t *how = lv_obj_get_user_data(l_wait);

		was_online = online;
		if (online) {
			lv_obj_add_flag(l_wait, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(how, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(l_wait, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(how, LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (now_ms - pps_t0 >= 1000) {
		pps = (s->packets - pps_n0) * 1000 / (now_ms - pps_t0);
		pps_n0 = s->packets;
		pps_t0 = now_ms;
		lv_label_set_text_fmt(l_pps, "%u /s", pps);
	}
	if (s->packets == seen_packets) {
		return;
	}
	seen_packets = s->packets;

	lv_label_set_text(l_host, s->host_ip);
	lv_label_set_text_fmt(l_lost, "%u", s->lost);
	lv_obj_set_style_text_color(l_lost, ui_c(s->lost ? C_WARN : C_TEXT), 0);
	if (s->rtt_us >= 10000) {
		lv_label_set_text_fmt(l_rtt, "%u.%u ms", s->rtt_us / 1000, (s->rtt_us / 100) % 10);
	} else {
		lv_label_set_text_fmt(l_rtt, "%u us", s->rtt_us);
	}
	lv_label_set_text_fmt(l_cpu, "%u%%", (s->cpu_x10 + 5) / 10);
	lv_label_set_text_fmt(l_mem, "%u %%", s->mem_pct);
	lv_obj_set_width(bar_mem, MAX(s->mem_pct * 220 / 100, 2));
	fmt_rate(buf, sizeof(buf), "\xE2\x86\x93", s->rx_bps);
	lv_label_set_text(l_rx, buf);
	fmt_rate(buf, sizeof(buf), "\xE2\x86\x91", s->tx_bps);
	lv_label_set_text(l_tx, buf);
	lv_label_set_text_fmt(l_map_cap, "%u CPU CORES  LAST 25 S  ONE ROW PER CORE", s->n_cores);
	if (rtt_scale_us >= 1000) {
		lv_label_set_text_fmt(l_rtt_scale, "ROUND TRIP, MEASURED BY THE HOST  FULL SCALE %u MS", rtt_scale_us / 1000);
	} else {
		lv_label_set_text_fmt(l_rtt_scale, "ROUND TRIP, MEASURED BY THE HOST  FULL SCALE %u US", rtt_scale_us);
	}
	lv_obj_invalidate(map);
	lv_obj_invalidate(rtt_plot);
}

static void mini(const struct raster *r)
{
	static int32_t y[160];
	const struct hl_state *s = hostlink_state();
	int w = MIN(lv_area_get_width(&r->box), (int)ARRAY_SIZE(y));
	int h = lv_area_get_height(&r->box);

	if (!hostlink_online()) {
		/* no host: a flat dashed baseline, not a made-up trace */
		raster_hline_dash(r, r->box.x1, r->box.x1 + w - 1, r->box.y2 - 2, C_LINE2, 255, 4, 4);
		return;
	}
	for (int i = 0; i < w; i++) {
		int idx = (s->head + (i * HL_HISTORY) / w) % HL_HISTORY;

		y[i] = ((r->box.y2 - 1) << RQ) - (int32_t)(MIN(s->cpu_hist[idx], 1000) * ((h - 3) << RQ) / 1000);
	}
	raster_area(r, r->box.x1, y, w, r->box.y1, r->box.y2, C_AZURE, 80);
	raster_trace(r, r->box.x1, y, w, C_AZURE, 11, 255);
}

const struct screen screen_hostlink = {
	.title = "Host Link",
	.blurb = "A PC, over Ethernet",
	.create = create,
	.tick = tick,
	.mini = mini,
	.available = hostlink_online,
};
