/*
 * Shell.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "platform/gfx.h"
#include "platform/net.h"
#include "platform/touch.h"
#include "shell.h"
#include "theme.h"

#define MAX_SCREENS 12
#define HUD_W       212
#define HUD_H       86
#define HUD_BARS    96

static const struct screen *const *screens;
static int n_screens;
static int current = -1;

static lv_obj_t *roots[MAX_SCREENS];
static lv_obj_t *bar_title, *bar_clock, *bar_ip, *bar_fps, *bar_dot;
static lv_obj_t *dots[MAX_SCREENS];
static lv_obj_t *hud, *hud_text, *hud_graph;
static bool hud_on;

/* ---- status bar --------------------------------------------------------- */

static void build_bar(lv_obj_t *scr)
{
	lv_obj_t *bar = ui_box(scr, 0, 0, GFX_W, UI_BAR_H);

	lv_obj_set_style_bg_color(bar, ui_c(C_BG0), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(bar, ui_c(C_LINE), 0);
	lv_obj_set_style_border_width(bar, 1, 0);
	lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);

	lv_obj_t *mark = ui_label(bar, &font_head_20, C_AZURE, "N6");

	lv_obj_align(mark, LV_ALIGN_LEFT_MID, UI_PAD, 0);

	bar_title = ui_label(bar, &font_textm_16, C_TEXT, "");
	lv_obj_align(bar_title, LV_ALIGN_LEFT_MID, UI_PAD + 34, 0);

	bar_clock = ui_label(bar, &font_mono_14, C_TEXT, "00:00:00");
	lv_obj_align(bar_clock, LV_ALIGN_RIGHT_MID, -UI_PAD, 0);

	bar_fps = ui_label(bar, &font_mono_14, C_TEXT2, "-- fps");
	lv_obj_align(bar_fps, LV_ALIGN_RIGHT_MID, -UI_PAD - 92, 0);

	bar_ip = ui_label(bar, &font_mono_14, C_TEXT2, "no link");
	lv_obj_align(bar_ip, LV_ALIGN_RIGHT_MID, -UI_PAD - 176, 0);

	bar_dot = ui_box(bar, 0, 0, 8, 8);
	lv_obj_set_style_radius(bar_dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(bar_dot, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar_dot, ui_c(C_TEXT3), 0);
	lv_obj_align_to(bar_dot, bar_ip, LV_ALIGN_OUT_LEFT_MID, -8, 0);

	/* page dots, centred */
	int total = n_screens * 14 - 6;

	for (int i = 0; i < n_screens; i++) {
		dots[i] = ui_box(bar, GFX_W / 2 - total / 2 + i * 14, UI_BAR_H / 2 - 3, 6, 6);
		lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(dots[i], ui_c(C_LINE2), 0);
	}
}

static void update_bar(uint32_t now_ms)
{
	static uint32_t last;
	static char prev_ip[20];
	const struct gfx_stats *gs = gfx_stats();
	struct net_status ns;
	char buf[24];

	if (now_ms - last < 250) {
		return;
	}
	last = now_ms;

	uint32_t s = now_ms / 1000;

	snprintf(buf, sizeof(buf), "%02u:%02u:%02u", (s / 3600) % 100, (s / 60) % 60, s % 60);
	lv_label_set_text(bar_clock, buf);

	snprintf(buf, sizeof(buf), "%2u.%u fps", gs->fps_x10 / 10, gs->fps_x10 % 10);
	lv_label_set_text(bar_fps, buf);

	net_get_status(&ns);
	const char *ip = ns.have_ip ? ns.ip : (ns.link_up ? "dhcp..." : "no link");

	if (strcmp(ip, prev_ip) != 0) {
		strncpy(prev_ip, ip, sizeof(prev_ip) - 1);
		lv_label_set_text(bar_ip, ip);
		lv_obj_align(bar_ip, LV_ALIGN_RIGHT_MID, -UI_PAD - 176, 0);
		lv_obj_set_style_bg_color(bar_dot,
					  ui_c(ns.have_ip ? C_OK : (ns.link_up ? C_WARN : C_TEXT3)), 0);
		lv_obj_align_to(bar_dot, bar_ip, LV_ALIGN_OUT_LEFT_MID, -8, 0);
	}
}

/* ---- performance HUD ---------------------------------------------------- */

static void hud_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_obj_t *obj = lv_event_get_target(e);
	const struct gfx_stats *gs = gfx_stats();
	lv_area_t a;
	lv_draw_rect_dsc_t dsc;

	lv_obj_get_coords(obj, &a);
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_opa = LV_OPA_COVER;

	int32_t h = lv_area_get_height(&a);
	/* full scale: two refresh periods */
	uint32_t full = gs->period_us * 2;

	for (int i = 0; i < HUD_BARS; i++) {
		int idx = (gs->head + GFX_HISTORY - HUD_BARS + i) % GFX_HISTORY;
		uint32_t r = gs->render_us[idx];
		int32_t bh = (int32_t)MIN((uint64_t)r * h / full, (uint64_t)h);
		lv_area_t b = {a.x1 + i * 2, a.y2 - MAX(bh, 1) + 1, a.x1 + i * 2, a.y2};

		dsc.bg_color = ui_c(r > gs->period_us ? C_BAD : (r > gs->period_us * 3 / 4 ? C_WARN : C_AZURE));
		lv_draw_rect(layer, &dsc, &b);
	}
	/* budget line at one refresh period */
	lv_area_t line = {a.x1, a.y1 + h / 2, a.x2, a.y1 + h / 2};

	dsc.bg_color = ui_c(C_TEXT3);
	dsc.bg_opa = LV_OPA_60;
	lv_draw_rect(layer, &dsc, &line);
}

static void build_hud(void)
{
	hud = ui_box(lv_layer_top(), GFX_W - HUD_W - 8, UI_BAR_H + 8, HUD_W, HUD_H);
	lv_obj_set_style_bg_color(hud, ui_c(0x000000), 0);
	lv_obj_set_style_bg_opa(hud, LV_OPA_80, 0);
	lv_obj_set_style_border_color(hud, ui_c(C_LINE2), 0);
	lv_obj_set_style_border_width(hud, 1, 0);
	lv_obj_set_style_radius(hud, 6, 0);

	hud_text = ui_label(hud, &font_mono_12, C_TEXT, "");
	lv_obj_set_pos(hud_text, 10, 6);

	hud_graph = ui_box(hud, 10, 40, HUD_BARS * 2, HUD_H - 48);
	lv_obj_add_event_cb(hud_graph, hud_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_flag(hud, LV_OBJ_FLAG_HIDDEN);
}

static void update_hud(uint32_t now_ms)
{
	static uint32_t last_text;
	const struct gfx_stats *gs = gfx_stats();

	if (!hud_on) {
		return;
	}
	lv_obj_invalidate(hud_graph);
	if (now_ms - last_text >= 250) {
		last_text = now_ms;
		lv_label_set_text_fmt(hud_text, "%2u.%u fps  render %u.%u ms\nworst %u.%u ms  drop %u",
				      gs->fps_x10 / 10, gs->fps_x10 % 10,
				      gs->avg_render_us / 1000, (gs->avg_render_us / 100) % 10,
				      gs->worst_render_us / 1000, (gs->worst_render_us / 100) % 10,
				      gs->dropped);
	}
}

void shell_toggle_hud(void)
{
	hud_on = !hud_on;
	if (hud_on) {
		gfx_stats_reset_worst();
		lv_obj_remove_flag(hud, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(hud, LV_OBJ_FLAG_HIDDEN);
	}
}

/* ---- screens ------------------------------------------------------------ */

/* Edge swipes. Full-canvas screens (plots, the touch lab) use horizontal drags
 * themselves, so navigation only listens to touches that start within
 * EDGE_ZONE px of the left or right edge and travel inward.
 */
#define EDGE_ZONE   28
#define EDGE_TRAVEL 90

static bool edge_tracking, edge_fired;
static int16_t edge_x0;

bool shell_gesture_active(void)
{
	return edge_tracking;
}

static void update_edge_swipe(void)
{
	static bool was_down;
	struct touch_state ts;
	int n_down = 0;
	const struct touch_point *p = NULL;

	touch_get(&ts);
	for (int i = 0; i < TOUCH_MAX_POINTS; i++) {
		if (ts.pt[i].down) {
			n_down++;
			p = p ? p : &ts.pt[i];
		}
	}
	if (n_down == 1 && !was_down) {
		edge_tracking = p->x < EDGE_ZONE || p->x >= GFX_W - EDGE_ZONE;
		edge_fired = false;
		edge_x0 = p->x;
	}
	if (n_down == 1 && edge_tracking && !edge_fired) {
		int dx = p->x - edge_x0;

		if (edge_x0 >= GFX_W - EDGE_ZONE && dx < -EDGE_TRAVEL) {
			edge_fired = true;
			shell_goto((current + 1) % n_screens);
		} else if (edge_x0 < EDGE_ZONE && dx > EDGE_TRAVEL) {
			edge_fired = true;
			shell_goto((current + n_screens - 1) % n_screens);
		}
	}
	if (n_down == 0) {
		edge_tracking = false;
	}
	was_down = n_down > 0;
}

static void home_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	shell_goto(0);
}

void shell_goto(int index)
{
	if (index < 0 || index >= n_screens || index == current) {
		return;
	}
	if (current >= 0) {
		if (screens[current]->hide) {
			screens[current]->hide();
		}
		lv_obj_add_flag(roots[current], LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_style_bg_color(dots[current], ui_c(C_LINE2), 0);
	}
	current = index;
	lv_obj_remove_flag(roots[current], LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_bg_color(dots[current], ui_c(C_AZURE), 0);
	lv_label_set_text(bar_title, screens[current]->title);
	if (screens[current]->show) {
		screens[current]->show();
	}
}

int shell_current(void)
{
	return current;
}

int shell_count(void)
{
	return n_screens;
}

const struct screen *shell_screen(int index)
{
	return (index >= 0 && index < n_screens) ? screens[index] : NULL;
}

void shell_init(const struct screen *const *list, int count)
{
	lv_obj_t *scr = lv_screen_active();

	screens = list;
	n_screens = MIN(count, MAX_SCREENS);

	ui_theme_init();
	for (int i = 0; i < n_screens; i++) {
		roots[i] = ui_box(scr, 0, UI_CONTENT_Y, GFX_W, UI_CONTENT_H);
		lv_obj_add_flag(roots[i], LV_OBJ_FLAG_HIDDEN);
		screens[i]->create(roots[i]);
	}
	build_bar(scr);
	build_hud();

	/* the wordmark and title are the way home: a generous invisible button */
	lv_obj_t *home = ui_box(scr, 0, 0, 240, UI_BAR_H + 8);

	lv_obj_add_flag(home, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(home, home_cb, LV_EVENT_CLICKED, NULL);
	shell_goto(0);
}

void shell_tick(void)
{
	uint32_t now = k_uptime_get_32();

	if (touch_user_button_pressed()) {
		shell_toggle_hud();
	}
	update_edge_swipe();
	update_bar(now);
	update_hud(now);
	for (int i = 0; i < n_screens; i++) {
		if (screens[i]->service) {
			screens[i]->service(now);
		}
	}
	if (current >= 0 && screens[current]->tick) {
		screens[current]->tick(now);
	}
}
