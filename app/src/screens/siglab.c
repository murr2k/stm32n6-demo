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
 * With the MICROPHONE source the microphone goes to the headphones through a
 * five-band parametric equalizer (dsp/monitor.h), to judge it by ear. The
 * header gets LISTEN (the headphones, off whenever the screen shows), GAIN
 * and EQ (in or bypassed; held: every band flat); the spectrum, as tall as
 * the instrument's, shows what is heard, with the equalizer's curve over it
 * and a handle per band that the fingers drag.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "dsp/monitor.h"
#include "dsp/sig.h"
#include "dsp/theremin.h"
#include "platform/touch.h"
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

static bool visible, thm_mode, mon_mode, ghost_by_attract, captions_due;
static int sound_shown = -1;
static int pre_attract_source = -1;
static struct thm_status thm; /* this frame's copy, for the draw handlers */
static struct mon_status mon; /* the same for the monitor */

enum { M_ANALYSER, M_INSTRUMENT, M_MONITOR };
static void set_mode(int m);

/* The instrument and the monitor take the tall spectrum. */
static inline bool tall(void)
{
	return thm_mode || mon_mode;
}

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

/* ---- the monitor's equalizer, played with the fingers -----------------------
 * Each band has a handle on the spectrum: across is its frequency (the
 * spectrum's own axis), up and down its gain on the equalizer's own scale
 * (0 dB in the middle, +/-15 dB near the edges). A finger that comes down
 * near a handle takes it and drags it from where it took it (the handle does
 * not jump to the finger); a second finger that comes down near a held
 * handle sets that band's width by its distance from the first, apart being
 * wider (lower Q). Fingers that come down anywhere else (a button, an edge
 * swipe) or were down when the screen showed do nothing. The touch table is
 * read once per frame on the UI thread; the sound follows within a block.
 */
#define EQ_GRAB    56                       /* px from a handle within which a finger takes it */
#define EQ_WIDTH_R 150                      /* px from a held handle within which a second finger sets its width */
#define EQ_SPAN    ((SPEC_H_T - 3) / 2 - 14) /* px from 0 dB to MON_DB_MAX */

enum { F_UP, F_DRAG, F_WIDTH, F_IGNORED };
struct finger {
	uint8_t state;
	int8_t band;
	float dx, dy; /* handle minus finger when it was taken */
	float q0, d0; /* width: the band's Q and the fingers' distance then */
};
static struct finger fing[TOUCH_MAX_POINTS];
static bool eq_fresh, eq_touched;
static int16_t eq_cdb[SIG_COLS]; /* the equalizer's response, 0.01 dB */
static uint32_t eq_curve_ver = UINT32_MAX;

static float eq_x(float hz)
{
	return (float)PX + sig_hz_colf(hz);
}

static float eq_yc(void)
{
	return (float)(UI_CONTENT_Y + SPEC_Y + SPEC_H_T / 2);
}

static float eq_y(float db)
{
	return eq_yc() - db * ((float)EQ_SPAN / MON_DB_MAX);
}

static int finger_on(int band, int state)
{
	for (int s = 0; s < TOUCH_MAX_POINTS; s++) {
		if (fing[s].state == state && fing[s].band == band) {
			return s;
		}
	}
	return -1;
}

static void eq_touch(void)
{
	struct touch_state ts;
	struct mon_band nb[MON_BANDS];
	bool changed = false;
	/* where a finger may come down: the spectrum, clear of the edge-swipe zones */
	int x1 = MAX(PX, EDGE), x2 = MIN(PX + SIG_COLS - 1, GFX_W - 1 - EDGE);
	int y1 = UI_CONTENT_Y + SPEC_Y, y2 = y1 + SPEC_H_T - 1;

	touch_get(&ts);
	memcpy(nb, mon.band, sizeof(nb));
	if (eq_fresh) {
		/* just shown: whatever is down already came down before */
		for (int s = 0; s < TOUCH_MAX_POINTS; s++) {
			fing[s].state = ts.pt[s].down ? F_IGNORED : F_UP;
		}
		eq_fresh = false;
	}
	for (int s = 0; s < TOUCH_MAX_POINTS; s++) {
		const struct touch_point *p = &ts.pt[s];
		struct finger *f = &fing[s];

		if (!p->down) {
			f->state = F_UP;
			continue;
		}
		if (f->state == F_UP) {
			f->state = F_IGNORED;
			if (shell_gesture_active() || p->x < x1 || p->x > x2 || p->y < y1 || p->y > y2) {
				continue;
			}
			/* near a handle another finger holds: that band's width */
			int other = -1;
			float best = EQ_WIDTH_R;

			for (int o = 0; o < TOUCH_MAX_POINTS; o++) {
				int b = fing[o].band;

				if (o == s || fing[o].state != F_DRAG || finger_on(b, F_WIDTH) >= 0) {
					continue;
				}
				float d = hypotf((float)p->x - eq_x(nb[b].hz), (float)p->y - eq_y(nb[b].db));

				if (d < best) {
					best = d;
					other = o;
				}
			}
			if (other >= 0) {
				f->state = F_WIDTH;
				f->band = fing[other].band;
				f->q0 = nb[f->band].q;
				f->d0 = MAX(fabsf((float)(p->x - ts.pt[other].x)), 12.0f);
				continue;
			}
			/* otherwise the nearest handle nobody holds */
			int band = -1;

			best = EQ_GRAB;
			for (int b = 0; b < MON_BANDS; b++) {
				if (finger_on(b, F_DRAG) >= 0) {
					continue;
				}
				float d = hypotf((float)p->x - eq_x(nb[b].hz), (float)p->y - eq_y(nb[b].db));

				if (d < best) {
					best = d;
					band = b;
				}
			}
			if (band >= 0) {
				f->state = F_DRAG;
				f->band = (int8_t)band;
				f->dx = eq_x(nb[band].hz) - (float)p->x;
				f->dy = eq_y(nb[band].db) - (float)p->y;
				eq_touched = true;
			}
			continue;
		}
		if (f->state == F_DRAG) {
			int b = f->band;
			float x = CLAMP((float)p->x + f->dx, (float)PX, (float)(PX + SIG_COLS - 1));
			float y = CLAMP((float)p->y + f->dy, eq_y(MON_DB_MAX), eq_y(-MON_DB_MAX));
			float db = (eq_yc() - y) * (MON_DB_MAX / (float)EQ_SPAN);

			nb[b].hz = sig_colf_hz(x - (float)PX);
			nb[b].db = fabsf(db) < 0.4f ? 0.0f : db; /* a detent at 0 dB: a band can be flattened by finger */
			changed = true;
		} else if (f->state == F_WIDTH) {
			int o = finger_on(f->band, F_DRAG);

			if (o < 0) {
				f->state = F_IGNORED; /* the first finger lifted: this one is done */
				continue;
			}
			float d = MAX(fabsf((float)(p->x - ts.pt[o].x)), 12.0f);

			nb[f->band].q = f->q0 * f->d0 / d;
			changed = true;
		}
	}
	if (changed) {
		for (int b = 0; b < MON_BANDS; b++) {
			if (memcmp(&nb[b], &mon.band[b], sizeof(nb[b])) != 0) {
				monitor_set_band(b, nb[b].hz, nb[b].db, nb[b].q);
			}
		}
	}
}

/* The equalizer over the spectrum: its 0 dB line and scale on the right, its
 * response, a handle per band (a readout on the held ones), and above it all
 * what the headphones get. */
static void draw_eq(const struct raster *r, lv_layer_t *layer)
{
	static const int marks[] = {12, 6, -6, -12};
	int yc = r->box.y1 + SPEC_H_T / 2;
	uint32_t col = mon.eq ? C_MINT : C_TEXT3;
	char buf[64], hz[16];

	raster_hline_dash(r, r->box.x1, r->box.x2, yc, C_MINT, 90, 6, 4);
	for (size_t i = 0; i < ARRAY_SIZE(marks); i++) {
		int y = yc - marks[i] * EQ_SPAN / (int)MON_DB_MAX;

		raster_fill_a(r, r->box.x2 - 6, y, r->box.x2, y, C_MINT, 160);
		snprintf(buf, sizeof(buf), "%+d", marks[i]);
		ui_draw_text(layer, &font_mono_12, C_TEXT3, r->box.x2 - 10 - ui_text_width(&font_mono_12, buf), y - 7,
			     buf);
	}
	ui_draw_text(layer, &font_mono_12, C_TEXT3, r->box.x2 - 46, yc - 15, "EQ 0");

	for (int c = 0; c < SIG_COLS; c++) {
		int32_t y = yc * RQ_ONE + RQ_ONE / 2 - (int32_t)eq_cdb[c] * (EQ_SPAN * RQ_ONE) / (int32_t)(MON_DB_MAX * 100);

		ybuf2[c] = CLAMP(y, (r->box.y1 + 1) * RQ_ONE, (r->box.y2 - 1) * RQ_ONE);
	}
	raster_trace(r, r->box.x1, ybuf2, SIG_COLS, col, 14, mon.eq ? 255 : 160);

	for (int b = 0; b < MON_BANDS; b++) {
		/* eq_x()/eq_y() are panel coordinates, as the touch is; the box is where the
		 * pane is drawn */
		int x = r->box.x1 + (int)(eq_x(mon.band[b].hz) + 0.5f) - PX;
		int y = r->box.y1 + (int)(eq_y(mon.band[b].db) + 0.5f) - (UI_CONTENT_Y + SPEC_Y);
		bool held = finger_on(b, F_DRAG) >= 0;
		int cx = x * RQ_ONE + RQ_ONE / 2, cy = y * RQ_ONE + RQ_ONE / 2;

		if (held) {
			raster_fill_a(r, x, r->box.y1, x, r->box.y2, col, 90);
		}
		raster_ring(r, cx, cy, 11 * RQ_ONE, 2 * RQ_ONE, col, held ? 255 : 170);
		raster_disc(r, cx, cy, 3 * RQ_ONE, col, 255);
		snprintf(buf, sizeof(buf), "%d", b + 1);
		ui_draw_text(layer, &font_mono_12, held ? C_TEXT : C_TEXT2, x - 3, MAX(y - 30, r->box.y1 + 2), buf);
		if (!held) {
			continue;
		}
		fmt_hz(hz, sizeof(hz), mon.band[b].hz);
		snprintf(buf, sizeof(buf), "%s  %s  %+.1f dB  Q %.2f", monitor_band_name(b), hz, (double)mon.band[b].db,
			 (double)mon.band[b].q);
		int w = ui_text_width(&font_mono_12, buf), tx = x + 18, ty = CLAMP(y - 26, r->box.y1 + 40, r->box.y2 - 18);

		if (tx + w > r->box.x2 - 40) {
			tx = x - 18 - w;
		}
		raster_fill_a(r, tx - 5, ty - 2, tx + w + 4, ty + 14, C_BG2, 225);
		ui_draw_text(layer, &font_mono_12, C_TEXT, tx, ty, buf);
	}

	/* what the headphones get, top right */
	if (!mon.listen) {
		snprintf(buf, sizeof(buf), "HEADPHONES OFF  TAP LISTEN");
	} else if (mon.reduce_db < -0.5f) {
		snprintf(buf, sizeof(buf), "LIMITER %.0f dB  DELAY %u MS", (double)mon.reduce_db,
			 (unsigned int)((mon.delay_us + 500) / 1000));
	} else {
		snprintf(buf, sizeof(buf), "%sDELAY %u MS", mon.eq ? "" : "EQ BYPASSED  ",
			 (unsigned int)((mon.delay_us + 500) / 1000));
	}
	int w = ui_text_width(&font_mono_12, buf);

	ui_draw_text(layer, &font_mono_12, mon.listen ? C_TEXT3 : C_AMBER, r->box.x2 - 8 - w, r->box.y1 + 8, buf);

	if (!eq_touched) {
		const char *hint = "Drag a point: across is frequency, up and down is gain; a second finger beside it sets the width";

		w = ui_text_width(&font_text_14, hint);
		ui_draw_text(layer, &font_text_14, C_TEXT2, (r->box.x1 + r->box.x2 - w) / 2, r->box.y2 - 44, hint);
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
	if (tall()) {
		/* the instrument and the monitor: a glow under the spectrum, not a fill to
		 * the floor (five loud voices filled most of the tall pane: 12 ms frames) */
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
	} else if (mon_mode) {
		draw_eq(&r, layer);
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

/* The split of the plot pane: analyser, or instrument or monitor with a tall
 * spectrum. Only these numbers change between them (no object moves or
 * resizes). */
static int spec_h(void)
{
	return tall() ? SPEC_H_T : SPEC_H;
}

static int wf_off(void)
{
	return (tall() ? WF_Y_T : WF_Y) - SPEC_Y;
}

static int wf_h(void)
{
	return tall() ? WF_H_T : SIG_WF_H;
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

	if (tall()) {
		return; /* the theremin and the equalizer read the touch panel themselves, every finger */
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

/* The header's three middle boxes: WAVE, SNAP and SOUND for the theremin;
 * GAIN, EQ and LISTEN for the monitor. */
static void wave_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (thm_mode) {
		theremin_set_wave(thm.wave + 1);
	} else if (mon_mode) {
		monitor_set_gain(mon.gain_db >= 40 ? 20 : mon.gain_db + 10); /* +20, +30, +40 dB */
	}
}

static void snap_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (thm_mode) {
		theremin_set_snap(!thm.snap);
	} else if (mon_mode) {
		monitor_set_eq(!mon.eq);
	}
}

static void eq_long(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (mon_mode) {
		monitor_flat();
	}
}

static void sound_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (thm_mode) {
		theremin_set_sound(!thm.sound);
	} else if (mon_mode) {
		monitor_set_listen(!mon.listen);
	}
}

/* The switch (the theremin's SOUND, the monitor's LISTEN) as it is now:
 * moves only when it changes. */
static void switch_update(bool on)
{
	if ((int)on == sound_shown) {
		return;
	}
	sound_shown = on;
	lv_obj_set_style_bg_color(sw_track, ui_c(on ? C_MINT : C_LINE2), 0);
	lv_obj_set_style_bg_color(sw_knob, ui_c(on ? C_TEXT : C_TEXT2), 0);
	lv_obj_set_x(sw_knob, on ? 18 : 2);
	lv_label_set_text_static(l_sound, on ? "ON" : "OFF");
	lv_obj_set_style_text_color(l_sound, ui_c(on ? C_MINT : C_TEXT2), 0);
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
	lv_obj_add_event_cb(tap_box(root, 494, 130, snap_click), eq_long, LV_EVENT_LONG_PRESSED, NULL);

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

	/* the instrument while THEREMIN is the source and the screen shows, the
	 * monitor with MICROPHONE */
	int s = sig_source();

	set_mode(!visible ? M_ANALYSER : s == SIG_SRC_THEREMIN ? M_INSTRUMENT : s == SIG_SRC_MIC ? M_MONITOR : M_ANALYSER);
	if (captions_due) {
		lv_label_set_text_static(cap_peak, thm_mode ? "SOUND  TAP" : mon_mode ? "LISTEN  TAP" : "PEAK");
		lv_label_set_text_static(cap_level, thm_mode ? "WAVE  TAP" : mon_mode ? "GAIN  TAP" : "LEVEL");
		lv_label_set_text_static(cap_fft, thm_mode ? "SNAP  TAP" : mon_mode ? "EQ  TAP  HOLD: FLAT" : "1024-PT FFT");
		show_if(l_peak, !tall());
		show_if(sw_track, tall());
		show_if(l_sound, tall());
		captions_due = false;
	}
	if (ghost_by_attract && !shell_in_attract()) {
		theremin_ghost(false); /* someone touched: their fingers play now */
		ghost_by_attract = false;
	}
	theremin_get(&thm);
	if (thm_mode) {
		switch_update(thm.sound);
	} else if (mon_mode) {
		monitor_get(&mon);
		eq_touch();
		monitor_get(&mon); /* with what the fingers just set */
		if (mon.version != eq_curve_ver) {
			monitor_curve(&mon, eq_cdb, SIG_COLS);
			eq_curve_ver = mon.version;
		}
		switch_update(mon.listen);
		if (mon.listen && !shell_in_attract()) {
			/* someone has the headphones on: no attract mode while they listen
			 * without touching (it would take the screen and the sound away) */
			shell_note_input();
		}
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
	} else if (mon_mode) {
		snprintf(buf, sizeof(buf), "%+d dB", mon.gain_db);
		lv_label_set_text(l_level, buf);
		lv_label_set_text_static(l_fft, mon.eq ? "ON" : "BYPASSED");
	} else {
		snprintf(buf, sizeof(buf), "%.1f dBFS", (double)v->rms_db);
		lv_label_set_text(l_level, buf);
		snprintf(buf, sizeof(buf), "%u us", v->fft_us);
		lv_label_set_text(l_fft, buf);
	}
}

/* One owner of the audio output at a time: the one leaving fades out and lets
 * go first, and only then does the other take it. Taking it at once would
 * cut the fade off and leave the one leaving waiting for a fade that never
 * runs. Every frame, from set_mode(). */
static void audio_owner(void)
{
	struct thm_status st;

	if (!thm_mode) {
		theremin_enable(false); /* a short fade, then the output is given back */
	}
	if (!mon_mode) {
		monitor_enable(false);
	}
	theremin_get(&st);
	if (thm_mode && !monitor_enabled()) {
		theremin_enable(true);
	}
	if (mon_mode && !st.enabled) {
		monitor_enable(true);
	}
}

/* Analyser, instrument or monitor. Cheap, so it may run in show() and hide()
 * (inside a screen switch's time budget): no LVGL object moves or resizes,
 * the plot pane's draw handler splits its area by the mode, and the captions
 * that change are relabelled by tick(). Resizing panes and moving labels
 * instead cost 2.1 ms of switch time, and an attract lap dropped a frame on
 * it. */
static void set_mode(int m)
{
	bool t = m == M_INSTRUMENT, mo = m == M_MONITOR;

	if (t != thm_mode || mo != mon_mode) {
		if (mon_mode && !mo) {
			monitor_set_listen(false); /* the headphones are off whenever the monitor comes back */
		}
		thm_mode = t;
		mon_mode = mo;
		captions_due = true;
		sound_shown = -1;
		cursor_col = -1;
		lv_obj_add_flag(l_cursor, LV_OBJ_FLAG_HIDDEN);
		if (t) {
			/* the spectrum's part of the pane, in panel coordinates */
			int x1 = PX, y1 = UI_CONTENT_Y + SPEC_Y;

			theremin_set_field(x1, y1, x1 + SIG_COLS - 1, y1 + SPEC_H_T - 1, MAX(x1, EDGE),
					   MIN(x1 + SIG_COLS - 1, GFX_W - 1 - EDGE));
		}
		if (mo) {
			eq_fresh = true;
			eq_touched = false;
			eq_curve_ver = UINT32_MAX;
		}
	}
	audio_owner();
}

static void show(void)
{
	int s = sig_source();

	visible = true;
	set_mode(s == SIG_SRC_THEREMIN ? M_INSTRUMENT : s == SIG_SRC_MIC ? M_MONITOR : M_ANALYSER);
}

static void hide(void)
{
	visible = false;
	set_mode(M_ANALYSER);
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
	set_mode(M_INSTRUMENT);
}

static void service(uint32_t now_ms)
{
	/* The analysis feeds this screen and Home's thumbnail of it, nothing else.
	 * Run on every screen it cost each of them 0.8 ms a frame (2.9 ms worst,
	 * measured with svct on 2026-11-17). Skipped, it loses nothing: when it
	 * runs again it takes at most the last 50 ms of the generator and the
	 * latest of the microphone or the theremin, as after any stall. */
	if (visible || shell_screen(shell_current()) == &screen_home) {
		sig_service(now_ms);
	}
	theremin_service(); /* measures the DAC's rate: cheap, and wanted anywhere */
	monitor_service();
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
