/*
 * Signal Lab: triggered scope, log-frequency spectrum with peak hold and a
 * touch cursor, and a scrolling waterfall. One 1024-point FFT per frame.
 *
 * With the THEREMIN source the spectrum is an instrument (dsp/theremin.h): a
 * fingertip sounds the frequency of the column under it at the level of the
 * height it is at, so the peak sits under the finger. The level axis is then
 * a loudness taper instead of even dB steps, the spectrum grows into most of
 * the waterfall's height for finer levels, the waterfall keeps a strip that
 * draws the trail, each finger gets a crosshair and a readout, and a SOUND
 * switch takes PEAK's place in the header.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>

#include <zephyr/kernel.h>

#include "dsp/sig.h"
#include "dsp/theremin.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define PX       16
#define SCOPE_Y  44
#define SCOPE_H  56
#define SPEC_Y   108
#define SPEC_H   146
#define WF_Y     262
#define SPEC_H_T 252 /* theremin: the spectrum is the playing field */
#define WF_Y_T   368
#define WF_H_T   58
#define EDGE     28  /* the shell's edge-swipe zone: the field keeps clear of it */

static lv_obj_t *scope_pane, *plot_pane; /* plot: the spectrum above, the waterfall below */
static lv_obj_t *l_peak, *l_level, *l_fft, *l_hold, *l_cursor, *l_source, *cap_peak, *cap_level, *cap_fft;
static lv_obj_t *sw_track, *sw_knob, *l_sound; /* the theremin's SOUND switch */
static uint32_t lut[256];
static int cursor_col = -1, press_row;
static uint32_t cursor_until;
static int32_t ybuf[SIG_COLS], ybuf2[SIG_COLS];

static bool visible, thm_mode, ghost_by_attract, captions_due;
static int sound_shown = -1;
static int pre_attract_source = -1;
static struct thm_status thm; /* this frame's copy, for the draw handlers */

static void set_mode(bool t);

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

/* The spectrum's level axis: even dB steps (analyser), or the theremin's
 * loudness taper (instrument), which its touch reads back (theremin.c). */
static inline int32_t db_to_y(const lv_area_t *box, float db)
{
	float t = thm_mode ? 1.0f - thm_db_u(db) : CLAMP(db / SIG_DB_MIN, 0.0f, 1.0f);

	return (int32_t)((box->y1 + 1) * RQ_ONE + t * (lv_area_get_height(box) - 3) * RQ_ONE);
}

static void fmt_hz(char *buf, size_t n, float hz)
{
	if (hz >= 1000.0f) {
		snprintf(buf, n, "%.2f kHz", (double)(hz / 1000.0f));
	} else {
		snprintf(buf, n, "%.0f Hz", (double)hz);
	}
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

/* Theremin: semitone ticks along the top while SNAP is on, labelled at each C. */
static void draw_notes(const struct raster *r, lv_layer_t *layer)
{
	for (int m = 24; m <= 120; m++) { /* C1 to C9 */
		float hz = 440.0f * exp2f((m - 69) / 12.0f);
		int x = r->box.x1 + (int)(sig_hz_colf(hz) + 0.5f);

		if (x < r->box.x1 || x > r->box.x2) {
			continue;
		}
		bool c = m % 12 == 0;

		raster_fill_a(r, x, r->box.y1, x, r->box.y1 + (c ? 8 : 3), C_TEXT2, c ? 200 : 120);
		if (c) {
			char buf[6];

			snprintf(buf, sizeof(buf), "C%d", m / 12 - 1);
			ui_draw_text(layer, &font_mono_12, C_TEXT3, x + 3, r->box.y1 + 6, buf);
		}
	}
}

/* Theremin: a crosshair and a readout for every voice that sounds. */
static void draw_voices(const struct raster *r, lv_layer_t *layer)
{
	uint32_t color = thm.mode == THM_GHOST ? C_VIOLET : C_TEXT;
	bool any = false;
	char buf[48], hz[16], note[12];
	int held = 0, placed = 0;
	lv_area_t used[THM_VOICES + 1]; /* labels so far, and the latency figure */

	if (thm.lat_n > 0 && thm.mode == THM_TOUCH && thm.sound) {
		/* a touch's figure: meaningless while the ghost or a remote command plays,
		 * or with nothing to hear */
		snprintf(buf, sizeof(buf), "TOUCH TO SOUND %u MS", (unsigned int)((thm.lat_last_us + 500) / 1000));
		int w = ui_text_width(&font_mono_12, buf), x = r->box.x2 - 8 - w, y = r->box.y1 + 22;

		ui_draw_text(layer, &font_mono_12, C_TEXT3, x, y, buf);
		used[placed++] = (lv_area_t){x - 6, y - 3, x + w + 6, y + 15};
	}

	/* With three fingers or more each ring gets its note name only: five full
	 * readouts crowd the field. (They were suspected of the 16.5 ms frames of a
	 * five-voice chord; measured, all the overlay together is under 1 ms, and
	 * the cost was the spectrum's fill: see raster_glow().) */
	for (int v = 0; v < THM_VOICES; v++) {
		held += thm.v[v].on && thm.v[v].held;
	}
	for (int v = 0; v < THM_VOICES; v++) {
		const struct thm_voice_view *p = &thm.v[v];

		if (!p->on) {
			continue;
		}
		any = true;
		int x = r->box.x1 + (int)(sig_hz_colf(p->hz) + 0.5f);
		int32_t yq = db_to_y(&r->box, p->db);
		int y = yq >> RQ;

		raster_fill_a(r, x, r->box.y1, x, r->box.y2, color, p->held ? 110 : 50);
		raster_ring(r, x * RQ_ONE + RQ_ONE / 2, yq, 9 * RQ_ONE, 2 * RQ_ONE, color, p->held ? 255 : 120);
		raster_disc(r, x * RQ_ONE + RQ_ONE / 2, yq, 2 * RQ_ONE, color, 255);
		if (!p->held) {
			continue; /* a fading note keeps its mark, not its label */
		}
		theremin_note(p->hz, note, sizeof(note));
		if (held > 2) {
			snprintf(buf, sizeof(buf), "%s", note);
		} else {
			fmt_hz(hz, sizeof(hz), p->hz);
			snprintf(buf, sizeof(buf), "%s  %s  %.0f dB", note, hz, (double)p->db);
		}
		int w = ui_text_width(&font_mono_12, buf);
		int tx = x + 16, ty = CLAMP(y - 24, r->box.y1 + 4, r->box.y2 - 18);

		if (tx + w > r->box.x2 - 4) {
			tx = x - 16 - w;
		}
		/* two voices close in pitch: step the label down until it is clear */
		for (int k = 0; k < placed; k++) {
			if (tx - 5 <= used[k].x2 && tx + w + 4 >= used[k].x1 && ty - 2 <= used[k].y2 &&
			    ty + 14 >= used[k].y1) {
				ty = used[k].y2 + 3;
				k = -1; /* and check the others again */
				if (ty > r->box.y2 - 18) {
					break;
				}
			}
		}
		raster_fill_a(r, tx - 5, ty - 2, tx + w + 4, ty + 14, C_BG2, 225);
		ui_draw_text(layer, &font_mono_12, color, tx, ty, buf);
		used[placed++] = (lv_area_t){tx - 5, ty - 2, tx + w + 4, ty + 14};
	}
	if (!any && thm.mode == THM_TOUCH) {
		const char *hint = thm.sound ? "Touch to play: left to right is pitch, higher is louder"
					     : "Sound is off: touch to see it play, tap SOUND to hear it";
		int w = ui_text_width(&font_text_14, hint);

		ui_draw_text(layer, &font_text_14, C_TEXT2, (r->box.x1 + r->box.x2 - w) / 2,
			     (r->box.y1 + r->box.y2) / 2 - 8, hint);
	}
}

/* The spectrum, in the upper part of the plot pane (r.box is that part). */
static void spec_draw(struct raster r, lv_layer_t *layer)
{
	static const float grid_hz[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
	static const struct {
		float hz;
		const char *txt;
	} ticks[] = {{100, "100"}, {1000, "1k"}, {10000, "10k"}};
	/* level lines: every 20 dB as analyser; every 10 dB on the instrument's
	 * loudness taper, where they crowd towards the floor (-40 is the last that
	 * keeps room for its caption) */
	static const char *const db_txt[2][4] = {{"-20 dB", "-40 dB", "-60 dB", "-80 dB"},
						 {"-10 dB", "-20 dB", "-30 dB", "-40 dB"}};
	const float db_step = thm_mode ? -10.0f : -20.0f;
	const struct sig_view *v = sig_view();

	for (size_t i = 0; i < ARRAY_SIZE(grid_hz); i++) {
		int x = r.box.x1 + sig_hz_col(grid_hz[i]);
		bool decade = grid_hz[i] == 100 || grid_hz[i] == 1000 || grid_hz[i] == 10000;

		raster_fill(&r, x, r.box.y1, x, r.box.y2, decade ? C_LINE2 : C_LINE);
	}
	for (int i = 1; i <= 4; i++) {
		raster_hline_dash(&r, r.box.x1, r.box.x2, db_to_y(&r.box, db_step * i) >> RQ, C_LINE, 255, 2, 4);
	}
	raster_fill(&r, r.box.x1, r.box.y2, r.box.x2, r.box.y2, C_LINE2);
	/* the axis captions are drawn with the pane, not label objects: the pane is
	 * redrawn every frame anyway, and moving seven labels when the spectrum
	 * changes height cost 1.3 ms of switch time (style writes) */
	for (size_t i = 0; i < ARRAY_SIZE(ticks); i++) {
		ui_draw_text(layer, &font_mono_12, C_TEXT3, r.box.x1 + sig_hz_col(ticks[i].hz) + 4, r.box.y2 - 15,
			     ticks[i].txt);
	}
	for (int i = 0; i < 4; i++) {
		ui_draw_text(layer, &font_mono_12, C_TEXT3, r.box.x1 + 4,
			     (db_to_y(&r.box, db_step * (i + 1)) >> RQ) - 14, db_txt[thm_mode][i]);
	}

	for (int i = 0; i < SIG_COLS; i++) {
		ybuf[i] = db_to_y(&r.box, v->col_db[i]);
		ybuf2[i] = db_to_y(&r.box, v->col_peak[i]);
	}
	if (thm_mode) {
		/* the instrument: a glow under the spectrum, not a fill to the floor (five
		 * loud voices filled most of the tall pane: 12 ms frames) */
		raster_glow(&r, r.box.x1, ybuf, SIG_COLS, 28, r.box.y2 - 1, C_AZURE, 120);
	} else {
		raster_area(&r, r.box.x1, ybuf, SIG_COLS, r.box.y1, r.box.y2 - 1, C_AZURE, 110);
	}
	raster_trace(&r, r.box.x1, ybuf2, SIG_COLS, C_AMBER, 8, 150);
	raster_trace(&r, r.box.x1, ybuf, SIG_COLS, C_AZURE, 12, 255);

	if (thm_mode) {
		if (thm.snap) {
			draw_notes(&r, layer);
		}
		draw_voices(&r, layer);
	} else if (cursor_col >= 0) {
		int x = r.box.x1 + cursor_col;

		raster_fill_a(&r, x, r.box.y1, x, r.box.y2, C_TEXT, 150);
		raster_disc(&r, x * RQ_ONE + RQ_ONE / 2, ybuf[cursor_col], 4 * RQ_ONE, C_TEXT, 255);
	}
}

/* The waterfall, in the lower part of the plot pane. */
static void wf_draw(struct raster r)
{
	const struct sig_view *v = sig_view();

	if (v->waterfall == NULL) {
		return;
	}
	/* newest row on top: ring rows head..H-1 first, then 0..head-1, as many as fit */
	int h = lv_area_get_height(&r.box);
	int first = MIN(SIG_WF_H - v->wf_head, h);

	raster_lut8(&r, r.box.x1, r.box.y1, SIG_COLS, first, v->waterfall + v->wf_head * SIG_COLS, SIG_COLS,
		    lut);
	if (v->wf_head > 0 && first < h) {
		raster_lut8(&r, r.box.x1, r.box.y1 + first, SIG_COLS, MIN(v->wf_head, h - first), v->waterfall,
			    SIG_COLS, lut);
	}
	if (thm_mode) {
		for (int i = 0; i < THM_VOICES; i++) {
			if (thm.v[i].on && thm.v[i].held) {
				int x = r.box.x1 + (int)(sig_hz_colf(thm.v[i].hz) + 0.5f);

				raster_fill_a(&r, x, r.box.y1, x, r.box.y2, thm.mode == THM_GHOST ? C_VIOLET : C_TEXT,
					      90);
			}
		}
	} else if (cursor_col >= 0) {
		raster_fill_a(&r, r.box.x1 + cursor_col, r.box.y1, r.box.x1 + cursor_col, r.box.y2, C_TEXT,
			      90);
	}
}

/* The split of the plot pane: analyser, or instrument with a tall spectrum.
 * Only these numbers change between the two (no object moves or resizes). */
static int spec_h(void)
{
	return thm_mode ? SPEC_H_T : SPEC_H;
}

static int wf_off(void)
{
	return (thm_mode ? WF_Y_T : WF_Y) - SPEC_Y;
}

static int wf_h(void)
{
	return thm_mode ? WF_H_T : SIG_WF_H;
}

/* A part of the pane's raster, rows y1..y2. False if none of it is redrawn. */
static bool part(const struct raster *r, int y1, int y2, struct raster *s)
{
	*s = *r;
	s->box.y1 = y1;
	s->box.y2 = y2;
	s->clip.y1 = MAX(r->clip.y1, y1);
	s->clip.y2 = MIN(r->clip.y2, y2);
	return s->clip.y1 <= s->clip.y2;
}

static void plot_draw(lv_event_t *e)
{
	struct raster r, s;

	if (!raster_begin(&r, e)) {
		return;
	}
	if (part(&r, r.box.y1, r.box.y1 + spec_h() - 1, &s)) {
		spec_draw(s, lv_event_get_layer(e));
	}
	if (part(&r, r.box.y1 + wf_off(), r.box.y1 + wf_off() + wf_h() - 1, &s)) {
		wf_draw(s);
	}
}

/* Where on the plot pane the pointer is: its row from the pane's top, column. */
static void plot_point(lv_event_t *e, int *col, int *row)
{
	lv_point_t p;
	lv_area_t a;

	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(lv_event_get_current_target(e), &a);
	*col = CLAMP(p.x - a.x1, 0, SIG_COLS - 1);
	*row = p.y - a.y1;
}

static void wf_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	sig_hold(!sig_held());
	lv_label_set_text(l_hold, sig_held() ? "HOLD" : "RUN");
	lv_obj_set_style_text_color(l_hold, ui_c(sig_held() ? C_AMBER : C_MINT), 0);
}

static void plot_touch(lv_event_t *e)
{
	int col, row;

	if (thm_mode) {
		return; /* the theremin reads the touch panel itself, every finger */
	}
	plot_point(e, &col, &row);
	if (row < spec_h()) {
		cursor_col = col;
		cursor_until = k_uptime_get_32() + 4000;
	}
}

static void plot_pressed(lv_event_t *e)
{
	int col;

	plot_point(e, &col, &press_row);
}

static void plot_click(lv_event_t *e)
{
	int col, row;

	plot_point(e, &col, &row);
	/* tap the waterfall: run, hold. A quick theremin note that slides down off
	 * the field and lifts over the waterfall is not a tap on it. */
	if (row >= wf_off() && press_row >= wf_off()) {
		wf_click(e);
	}
}

static void source_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	/* generator, microphone (if it runs), theremin, generator */
	int s = sig_source();

	if (s == SIG_SRC_GENERATOR) {
		sig_set_source(SIG_SRC_MIC);
		if (sig_source() != SIG_SRC_MIC) {
			sig_set_source(SIG_SRC_THEREMIN);
		}
	} else {
		sig_set_source(s == SIG_SRC_MIC ? SIG_SRC_THEREMIN : SIG_SRC_GENERATOR);
	}
}

static void wave_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (thm_mode) {
		theremin_set_wave(thm.wave + 1);
	}
}

static void snap_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (thm_mode) {
		theremin_set_snap(!thm.snap);
	}
}

static void sound_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (thm_mode) {
		theremin_set_sound(!thm.sound);
	}
}

/* The SOUND switch as it is now: moves only when it changes. */
static void sound_switch_update(void)
{
	if ((int)thm.sound == sound_shown) {
		return;
	}
	sound_shown = thm.sound;
	lv_obj_set_style_bg_color(sw_track, ui_c(thm.sound ? C_MINT : C_LINE2), 0);
	lv_obj_set_style_bg_color(sw_knob, ui_c(thm.sound ? C_TEXT : C_TEXT2), 0);
	lv_obj_set_x(sw_knob, thm.sound ? 18 : 2);
	lv_label_set_text_static(l_sound, thm.sound ? "ON" : "OFF");
	lv_obj_set_style_text_color(l_sound, ui_c(thm.sound ? C_MINT : C_TEXT2), 0);
}

static void pill(lv_obj_t *o, uint32_t color)
{
	lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(o, ui_c(color), 0);
}

static void show_if(lv_obj_t *o, bool show)
{
	if (show) {
		lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
	}
}

static lv_obj_t *pane(lv_obj_t *root, int y, int h, lv_event_cb_t draw)
{
	lv_obj_t *p = ui_box(root, PX, y, SIG_COLS, h);

	lv_obj_add_event_cb(p, draw, LV_EVENT_DRAW_MAIN, NULL);
	return p;
}

static lv_obj_t *tap_box(lv_obj_t *root, int x, int w, lv_event_cb_t cb)
{
	lv_obj_t *b = ui_box(root, x, 0, w, 42);

	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(b, cb, LV_EVENT_SHORT_CLICKED, NULL);
	return b;
}

static void create(lv_obj_t *root)
{
	build_lut();

	lv_obj_set_pos(ui_caption(root, "SOURCE  TAP TO SWITCH"), PX, 8);
	l_source = ui_label(root, &font_mono_14, C_TEXT, sig_view()->source);
	lv_obj_set_pos(l_source, PX, 22);
	/* the two lines above are the button */
	tap_box(root, 0, 212, source_click);

	cap_peak = ui_caption(root, "PEAK");
	lv_obj_set_pos(cap_peak, 220, 8);
	l_peak = ui_label(root, &font_mono_14, C_AZURE, "-");
	lv_obj_set_pos(l_peak, 220, 22);
	/* the theremin's SOUND switch, in PEAK's place (every finger has its own
	 * readout there) */
	sw_track = ui_box(root, 220, 21, 34, 18);
	pill(sw_track, C_MINT);
	sw_knob = ui_box(sw_track, 18, 2, 14, 14);
	pill(sw_knob, C_TEXT);
	l_sound = ui_label(root, &font_mono_14, C_MINT, "ON");
	lv_obj_set_pos(l_sound, 262, 22);
	show_if(sw_track, false);
	show_if(l_sound, false);
	tap_box(root, 214, 136, sound_click);

	cap_level = ui_caption(root, "LEVEL");
	lv_obj_set_pos(cap_level, 360, 8);
	l_level = ui_label(root, &font_mono_14, C_TEXT, "-");
	lv_obj_set_pos(l_level, 360, 22);
	tap_box(root, 352, 140, wave_click);

	cap_fft = ui_caption(root, "1024-PT FFT");
	lv_obj_set_pos(cap_fft, 500, 8);
	l_fft = ui_label(root, &font_mono_14, C_TEXT, "-");
	lv_obj_set_pos(l_fft, 500, 22);
	tap_box(root, 494, 130, snap_click);

	l_hold = ui_label(root, &font_mono_14, C_MINT, "RUN");
	lv_obj_align(l_hold, LV_ALIGN_TOP_RIGHT, -PX, 22);
	lv_obj_t *cap = ui_caption(root, "TAP WATERFALL");

	lv_obj_align(cap, LV_ALIGN_TOP_RIGHT, -PX, 8);

	scope_pane = pane(root, SCOPE_Y, SCOPE_H, scope_draw);
	plot_pane = pane(root, SPEC_Y, WF_Y + SIG_WF_H - SPEC_Y, plot_draw);

	lv_obj_add_flag(plot_pane, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(plot_pane, plot_pressed, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(plot_pane, plot_touch, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(plot_pane, plot_click, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_set_pos(ui_caption(root, "SCOPE  16 MS  AUTO-RANGED"), PX + 4, SCOPE_Y + 2);

	l_cursor = ui_label(root, &font_mono_12, C_TEXT, "");
	lv_obj_set_style_bg_color(l_cursor, ui_c(C_BG2), 0);
	lv_obj_set_style_bg_opa(l_cursor, LV_OPA_90, 0);
	lv_obj_set_style_pad_hor(l_cursor, 6, 0);
	lv_obj_set_style_pad_ver(l_cursor, 3, 0);
	lv_obj_set_style_radius(l_cursor, 4, 0);
	lv_obj_add_flag(l_cursor, LV_OBJ_FLAG_HIDDEN);
}

static void tick(uint32_t now_ms)
{
	static uint32_t last_text;
	const struct sig_view *v = sig_view();
	char buf[40], hz[16];

	/* the instrument while THEREMIN is the source and the screen shows */
	set_mode(visible && sig_source() == SIG_SRC_THEREMIN);
	if (captions_due) {
		lv_label_set_text_static(cap_peak, thm_mode ? "SOUND  TAP" : "PEAK");
		lv_label_set_text_static(cap_level, thm_mode ? "WAVE  TAP" : "LEVEL");
		lv_label_set_text_static(cap_fft, thm_mode ? "SNAP  TAP" : "1024-PT FFT");
		show_if(l_peak, !thm_mode);
		show_if(sw_track, thm_mode);
		show_if(l_sound, thm_mode);
		captions_due = false;
	}
	if (ghost_by_attract && !shell_in_attract()) {
		theremin_ghost(false); /* someone touched: their fingers play now */
		ghost_by_attract = false;
	}
	theremin_get(&thm);
	if (thm_mode) {
		sound_switch_update();
	}

	lv_obj_invalidate(scope_pane);
	lv_obj_invalidate(plot_pane);

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
		int s = sig_source();

		lv_label_set_text_static(l_source, v->source);
		lv_obj_set_style_text_color(
			l_source, ui_c(s == SIG_SRC_MIC ? C_MINT : s == SIG_SRC_THEREMIN ? C_VIOLET : C_TEXT), 0);
	}
	if (!thm_mode) {
		fmt_hz(hz, sizeof(hz), v->peak_hz);
		lv_label_set_text(l_peak, hz);
	}
	if (thm_mode) {
		lv_label_set_text_static(l_level, theremin_wave_name(thm.wave));
		lv_label_set_text_static(l_fft, thm.snap ? "SEMITONES" : "OFF");
	} else {
		snprintf(buf, sizeof(buf), "%.1f dBFS", (double)v->rms_db);
		lv_label_set_text(l_level, buf);
		snprintf(buf, sizeof(buf), "%u us", v->fft_us);
		lv_label_set_text(l_fft, buf);
	}
}

/* Analyser or instrument. Cheap, so it may run in show() and hide() (inside a
 * screen switch's time budget): no LVGL object moves or resizes, the plot
 * pane's draw handler splits its area by thm_mode, and the two captions that
 * change are relabelled by tick(). Resizing panes and moving labels instead
 * cost 2.1 ms of switch time, and an attract lap dropped a frame on it. */
static void set_mode(bool t)
{
	if (t == thm_mode) {
		return;
	}
	thm_mode = t;
	captions_due = true;
	cursor_col = -1;
	lv_obj_add_flag(l_cursor, LV_OBJ_FLAG_HIDDEN);
	if (t) {
		/* the spectrum's part of the pane, in panel coordinates */
		int x1 = PX, y1 = UI_CONTENT_Y + SPEC_Y;

		theremin_set_field(x1, y1, x1 + SIG_COLS - 1, y1 + SPEC_H_T - 1, MAX(x1, EDGE),
				   MIN(x1 + SIG_COLS - 1, GFX_W - 1 - EDGE));
	}
	theremin_enable(t); /* off: a short fade, then the output is given back */
}

static void show(void)
{
	visible = true;
	set_mode(sig_source() == SIG_SRC_THEREMIN);
}

static void hide(void)
{
	visible = false;
	set_mode(false);
	if (ghost_by_attract) {
		theremin_ghost(false);
		ghost_by_attract = false;
	}
	if (pre_attract_source >= 0) {
		sig_set_source(pre_attract_source);
		pre_attract_source = -1;
	}
	if (sig_held()) {
		wf_click(NULL);
	}
}

/* Attract mode: a ghost plays the theremin, a flying saucer from a 1950s
 * film; the source the screen had comes back when it leaves. */
static void attract_start(void)
{
	pre_attract_source = sig_source();
	sig_set_source(SIG_SRC_THEREMIN);
	theremin_ghost(true);
	ghost_by_attract = true;
	set_mode(true);
}

static void service(uint32_t now_ms)
{
	sig_service(now_ms);
	theremin_service();
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
	.blurb = "Scope, spectrum, theremin",
	.create = create,
	.tick = tick,
	.show = show,
	.hide = hide,
	.service = service,
	.mini = mini,
	.attract_secs = 14,
	.attract_start = attract_start,
};
