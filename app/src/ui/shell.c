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

#define MAX_SCREENS 16
#define HUD_W       212
#define HUD_H       86
#define HUD_BARS    96

static const struct screen *const *screens;
static int n_screens;
static int current = -1;

static lv_obj_t *roots[MAX_SCREENS];
static lv_obj_t *bar_obj, *bar_title, *bar_clock, *bar_ip, *bar_fps, *bar_dot;
static lv_obj_t *dot_row, *dots[MAX_SCREENS];
static uint32_t dots_listed; /* the screens that had a dot at the last layout */
static lv_obj_t *hud, *hud_text, *hud_graph;
static bool hud_on;

/* ---- status bar --------------------------------------------------------- */

static void build_bar(lv_obj_t *scr)
{
	lv_obj_t *bar = ui_box(scr, 0, 0, GFX_W, UI_BAR_H);

	bar_obj = bar;

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

	/* page dots, centred; laid out by layout_dots() */
	dot_row = ui_box(bar, 0, UI_BAR_H / 2 - 3, n_screens * 14, 6);
	for (int i = 0; i < n_screens; i++) {
		dots[i] = ui_box(dot_row, i * 14, 0, 6, 6);
		lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(dots[i], ui_c(C_LINE2), 0);
	}
	dots_listed = UINT32_MAX;
}

bool shell_listed(int index)
{
	const struct screen *s = shell_screen(index);

	return s && (s->listed == NULL || s->listed() || index == current);
}

/* A dot for every listed screen, the row centred. Moves objects only when
 * the set changes (a screen whose data comes and goes). */
static void layout_dots(void)
{
	uint32_t set = 0;
	int k = 0;

	for (int i = 0; i < n_screens; i++) {
		set |= shell_listed(i) ? BIT(i) : 0;
	}
	if (set == dots_listed) {
		return;
	}
	dots_listed = set;
	for (int i = 0; i < n_screens; i++) {
		if (set & BIT(i)) {
			lv_obj_remove_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
			lv_obj_set_x(dots[i], k++ * 14);
		} else {
			lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
	lv_obj_set_x(dot_row, GFX_W / 2 - (k * 14 - 6) / 2);
}

/* lv_label_set_text() redraws the label even when the text has not changed: the
 * clock and the fps, set every 250 ms, cost Home 0.8 ms in every 15th frame
 * (zprof, 2026-11-22). */
static void set_if_changed(lv_obj_t *l, const char *text)
{
	if (strcmp(lv_label_get_text(l), text) != 0) {
		lv_label_set_text(l, text);
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
	layout_dots();

	uint32_t s = now_ms / 1000;

	snprintf(buf, sizeof(buf), "%02u:%02u:%02u", (s / 3600) % 100, (s / 60) % 60, s % 60);
	set_if_changed(bar_clock, buf);

	snprintf(buf, sizeof(buf), "%2u.%u fps", gs->fps_x10 / 10, gs->fps_x10 % 10);
	set_if_changed(bar_fps, buf);

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

static bool covered; /* a full-screen view is up: see shell_set_cover() */

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
	if (covered) {
		/* A full-screen view owns every touch. Without this, a drag that began
		 * near an edge (the left end of the video scrub bar is 14 px from it)
		 * changed screen and closed the film.
		 */
		edge_tracking = false;
		was_down = true; /* a contact still down when the view closes must not start a swipe */
		return;
	}
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

		int step = 0;

		if (edge_x0 >= GFX_W - EDGE_ZONE && dx < -EDGE_TRAVEL) {
			step = 1;
		} else if (edge_x0 < EDGE_ZONE && dx > EDGE_TRAVEL) {
			step = n_screens - 1;
		}
		if (step) {
			int to = current;

			edge_fired = true;
			do { /* past screens that are not listed now */
				to = (to + step) % n_screens;
			} while (to != current && !shell_listed(to));
			shell_goto(to);
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

/* ---- transitions and attract mode --------------------------------------- */

#define FADE_OUT_MS       110
#define FADE_IN_MS        200
#define FADE_HOLD_MS      34
#define ATTRACT_IDLE_MS   90000
#define ATTRACT_DWELL_MS  12000
#define ATTRACT_MAX_MS    75000   /* upper bound on a "busy" stay */

static lv_obj_t *bar_demo;
static bool fading;
static int fade_target = -1, queued = -1;
static bool attract;
static uint32_t last_input_ms, attract_next_ms, attract_enter_ms, seen_reports;
static const struct shell_program *program;
static bool prog_run;
static uint32_t prog_silent_ms;   /* since when the program's clock has not run */

#define PROG_WAIT_MS 5000         /* no soundtrack after this long: the screens take turns */

static void switch_to(int index);

/* The fade is done by the display controller (layer alpha), so it costs no
 * render time; the frame that swaps the screens and the new screen's first
 * full render both happen while the display is dark.
 */
static void fade_exec(void *var, int32_t v)
{
	ARG_UNUSED(var);
	gfx_set_fade((uint8_t)v);
}

/* The settle is done by the display controller too (the layer window): moved
 * by LVGL, the new screen's objects were all redrawn on every frame of it,
 * each a full-screen render while the screen was already fading in. */
static void settle(void *var, int32_t v)
{
	ARG_UNUSED(var);
	gfx_set_offset(v);
}

static void fade_in_done(lv_anim_t *a)
{
	ARG_UNUSED(a);
	fading = false;
	gfx_set_fade(255);
	if (queued >= 0 && queued != current) {
		int q = queued;

		queued = -1;
		shell_goto(q);
	}
	queued = -1;
}

static void fade_out_done(lv_anim_t *a)
{
	lv_anim_t in;

	ARG_UNUSED(a);
	switch_to(fade_target);

	/* fade back in while the new screen settles upwards by a few pixels */
	lv_anim_init(&in);
	lv_anim_set_var(&in, &fading);
	lv_anim_set_exec_cb(&in, fade_exec);
	lv_anim_set_values(&in, 0, 255);
	lv_anim_set_duration(&in, FADE_IN_MS);
	lv_anim_set_delay(&in, FADE_HOLD_MS);
	lv_anim_set_path_cb(&in, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&in, fade_in_done);
	lv_anim_start(&in);

	lv_anim_init(&in);
	lv_anim_set_var(&in, &fade_target);
	lv_anim_set_exec_cb(&in, settle);
	lv_anim_set_values(&in, 10, 0);
	lv_anim_set_duration(&in, FADE_IN_MS);
	lv_anim_set_delay(&in, FADE_HOLD_MS);
	lv_anim_set_path_cb(&in, lv_anim_path_ease_out);
	lv_anim_start(&in);
	gfx_set_offset(10);
}

void shell_goto(int index)
{
	lv_anim_t out;

	if (index < 0 || index >= n_screens || index == current) {
		return;
	}
	if (current < 0) {
		switch_to(index);
		return;
	}
	if (fading) {
		queued = index; /* go there once this transition has finished */
		return;
	}
	fading = true;
	fade_target = index;
	gfx_trace_begin(current, index);
	lv_anim_init(&out);
	lv_anim_set_var(&out, &fading);
	lv_anim_set_exec_cb(&out, fade_exec);
	lv_anim_set_values(&out, 255, 0);
	lv_anim_set_duration(&out, FADE_OUT_MS);
	lv_anim_set_path_cb(&out, lv_anim_path_ease_in);
	lv_anim_set_completed_cb(&out, fade_out_done);
	lv_anim_start(&out);
}
void shell_note_input(void)
{
	last_input_ms = k_uptime_get_32();
	if (attract) {
		attract = false;
		lv_obj_add_flag(bar_demo, LV_OBJ_FLAG_HIDDEN);
	}
}

void shell_note_presence(void)
{
	last_input_ms = k_uptime_get_32(); /* one 32-bit store: safe from any thread */
}

static int index_of(const struct screen *s)
{
	for (int i = 0; i < n_screens; i++) {
		if (screens[i] == s) {
			return i;
		}
	}
	return -1;
}

void shell_start_attract(void)
{
	attract = true;
	attract_next_ms = k_uptime_get_32();
	attract_enter_ms = attract_next_ms; /* the current screen may be busy: time it from now */
	lv_obj_remove_flag(bar_demo, LV_OBJ_FLAG_HIDDEN);
	lv_obj_align_to(bar_demo, bar_title, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

	prog_run = program && program->available && program->available() &&
		   index_of(program->steps[0].screen) >= 0;
	if (prog_run) {
		int first = index_of(program->steps[0].screen);

		prog_silent_ms = attract_next_ms;
		if (first == current) {
			program->start(); /* already there: no arrival to start it */
		} else {
			shell_goto(first); /* its attract_start starts the soundtrack on arrival */
		}
	}
}

bool shell_in_attract(void)
{
	return attract;
}

void shell_set_program(const struct shell_program *p)
{
	program = p;
}

bool shell_program_running(void)
{
	return attract && prog_run;
}

int32_t shell_program_ms(void)
{
	if (!shell_program_running()) {
		return -1;
	}
	int32_t t = program->clock_ms();

	return t < 0 ? -1 : (int32_t)((uint32_t)t % program->lap_ms);
}

uint32_t shell_program_length_ms(void)
{
	return shell_program_running() ? program->lap_ms : 0;
}

/* The program's screen for now: the step whose time has come, counting the
 * fade-out early, so the switch lands on the step's start. */
static void update_program(uint32_t now)
{
	int32_t t = program->clock_ms();

	if (t < 0) {
		if ((int32_t)(now - prog_silent_ms) > PROG_WAIT_MS) {
			prog_run = false; /* no soundtrack: the screens take turns as before */
			attract_next_ms = now;
		}
		return;
	}
	prog_silent_ms = now;
	if (fading) {
		return;
	}
	uint32_t at = ((uint32_t)t + FADE_OUT_MS) % program->lap_ms;
	int k = 0;

	for (int i = 0; i < program->n_steps; i++) {
		if (program->steps[i].at_ms <= at) {
			k = i;
		}
	}
	int idx = index_of(program->steps[k].screen);

	if (idx >= 0 && idx != current) {
		attract_enter_ms = now;
		shell_goto(idx);
	}
}

static void update_attract(uint32_t now)
{
	struct touch_state ts;

	touch_get(&ts);
	if (ts.reports != seen_reports) {
		seen_reports = ts.reports;
		shell_note_input();
	}
	if (!attract) {
		/* Signed: last_input_ms is stamped with a fresh clock reading and can be
		 * a millisecond AFTER 
ow, which was sampled at the top of the frame.
		 * Unsigned, that difference wrapped to four billion and started attract
		 * mode on a touch release, about one release in five.
		 */
		if ((int32_t)(now - last_input_ms) > ATTRACT_IDLE_MS) {
			shell_start_attract();
		}
		return;
	}
	if (prog_run) {
		update_program(now);
		if (prog_run) {
			return;
		}
	}
	if (fading || (int32_t)(now - attract_next_ms) < 0) {
		return;
	}
	const struct screen *cur = screens[current];

	if (cur->attract_busy && cur->attract_busy() && (int32_t)(now - attract_enter_ms) < ATTRACT_MAX_MS) {
		attract_next_ms = now + 500;
		return;
	}
	for (int step = 1; step <= n_screens; step++) {
		int i = (current + step) % n_screens;
		const struct screen *s = screens[i];

		if (s->attract_skip || (s->available && !s->available())) {
			continue;
		}
		attract_enter_ms = now;
		attract_next_ms = now + (s->attract_secs ? s->attract_secs * 1000 : ATTRACT_DWELL_MS);
		shell_goto(i);
		return;
	}
}

static void switch_to(int index)
{
	uint32_t t0 = gfx_now_us();

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
	if (attract) {
		lv_obj_align_to(bar_demo, bar_title, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
		if (screens[current]->attract_start) {
			screens[current]->attract_start();
		}
	}
	gfx_trace_switch(current, gfx_now_us() - t0);
}

int shell_current(void)
{
	return current;
}

void shell_set_cover(bool on)
{
	/* A full-screen view (the video player) sits on top of everything. LVGL
	 * cannot tell that it is opaque, so hide what is underneath rather than
	 * render it every frame for nothing.
	 */
	lv_obj_t *const under[] = {bar_obj, current >= 0 ? roots[current] : NULL};

	covered = on;

	for (size_t i = 0; i < ARRAY_SIZE(under); i++) {
		if (under[i] == NULL) {
			continue;
		}
		if (on) {
			lv_obj_add_flag(under[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(under[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
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
	layout_dots();
	build_hud();
	bar_demo = ui_label(bar_obj, &font_mono_12, C_AZURE, "DEMO  TOUCH TO TAKE OVER");
	lv_obj_set_style_text_letter_space(bar_demo, 1, 0);
	lv_obj_add_flag(bar_demo, LV_OBJ_FLAG_HIDDEN);

	/* the wordmark and title are the way home: a generous invisible button */
	lv_obj_t *home = ui_box(scr, 0, 0, 240, UI_BAR_H + 8);

	lv_obj_add_flag(home, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(home, home_cb, LV_EVENT_CLICKED, NULL);
	shell_goto(0);
}

struct cost {
	uint32_t worst, sum, n;
};
static struct cost cost_svc[MAX_SCREENS], cost_tick[MAX_SCREENS], cost_bar, cost_tele;

static void cost_add(struct cost *c, uint32_t us)
{
	c->worst = MAX(c->worst, us);
	c->sum += us;
	c->n++;
}

void shell_cost_tele(uint32_t us)
{
	cost_add(&cost_tele, us);
}

static int cost_fmt(char *out, size_t len, const char *name, int i, const struct cost *c)
{
	char key[16];

	snprintf(key, sizeof(key), i < 0 ? "%s" : "%s%d", name, i);
	return c->n ? snprintf(out, len, "%s:%u/%u ", key, c->worst, c->sum / c->n) : 0;
}

int shell_costs(char *out, size_t len)
{
	int k = cost_fmt(out, len, "tele", -1, &cost_tele);

	k += cost_fmt(out + k, len - k, "bar", -1, &cost_bar);
	for (int i = 0; i < n_screens && k < (int)len - 48; i++) {
		k += cost_fmt(out + k, len - k, "svc", i, &cost_svc[i]);
		k += cost_fmt(out + k, len - k, "tick", i, &cost_tick[i]);
	}
	return k;
}

void shell_costs_reset(void)
{
	memset(cost_svc, 0, sizeof(cost_svc));
	memset(cost_tick, 0, sizeof(cost_tick));
	memset(&cost_bar, 0, sizeof(cost_bar));
	memset(&cost_tele, 0, sizeof(cost_tele));
}

void shell_tick(void)
{
	uint32_t now = k_uptime_get_32();
	uint32_t t0 = gfx_now_us(), t1;

	if (touch_user_button_pressed()) {
		shell_toggle_hud();
	}
	update_edge_swipe();
	update_attract(now);
	update_bar(now);
	update_hud(now);
	t1 = gfx_now_us();
	cost_add(&cost_bar, t1 - t0);
	for (int i = 0; i < n_screens; i++) {
		if (screens[i]->service) {
			t0 = t1;
			screens[i]->service(now);
			t1 = gfx_now_us();
			cost_add(&cost_svc[i], t1 - t0);
		}
	}
	if (current >= 0 && screens[current]->tick) {
		t0 = t1;
		screens[current]->tick(now);
		cost_add(&cost_tick[current], gfx_now_us() - t0);
	}
}
