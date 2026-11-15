/*
 * Touch Lab: five-point multi-touch made visible. Trails, crosshairs,
 * distances, the panel's report rate and the measured time from a touch
 * report to the buffer swap that shows it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>

#include <zephyr/kernel.h>

#include "platform/touch.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define TRAIL   40
#define RIPPLES 12
#define RIPPLE_MS 520

static const uint32_t slot_color[TOUCH_MAX_POINTS] = {C_AZURE, C_AMBER, C_VIOLET, C_MINT, C_CORAL};

struct finger {
	bool down;
	int16_t tx[TRAIL], ty[TRAIL];
	uint8_t head, len;
	uint32_t seq;
};

struct ripple {
	int16_t x, y;
	uint32_t t0;
	uint32_t color;
	bool on;
};

static struct finger fingers[TOUCH_MAX_POINTS];
static struct ripple ripples[RIPPLES];
static struct touch_state ts;
static lv_obj_t *canvas, *l_contacts, *l_rate, *l_latency, *l_hint, *l_dist;
static lv_obj_t *l_xy[TOUCH_MAX_POINTS];
static int n_down;
static uint32_t last_activity_ms;
static uint32_t pending_report_us;
static bool pending;
static uint32_t latency_us; /* smoothed */
static uint32_t rate_hz;

static void add_ripple(int x, int y, uint32_t color, uint32_t now)
{
	int oldest = 0;

	for (int i = 0; i < RIPPLES; i++) {
		if (!ripples[i].on) {
			oldest = i;
			break;
		}
		if (ripples[i].t0 < ripples[oldest].t0) {
			oldest = i;
		}
	}
	ripples[oldest] = (struct ripple){.x = x, .y = y, .t0 = now, .color = color, .on = true};
}

static void canvas_draw(lv_event_t *e)
{
	struct raster r;
	uint32_t now = k_uptime_get_32();

	if (!raster_begin(&r, e)) {
		return;
	}
	/* reference grid: a dot every 40 px */
	for (int y = r.box.y1 + 22; y < r.box.y2; y += 40) {
		for (int x = r.box.x1 + 20; x < r.box.x2; x += 40) {
			raster_fill(&r, x, y, x + 1, y + 1, C_LINE);
		}
	}

	for (int i = 0; i < RIPPLES; i++) {
		if (!ripples[i].on) {
			continue;
		}
		uint32_t age = now - ripples[i].t0;

		if (age >= RIPPLE_MS) {
			ripples[i].on = false;
			continue;
		}
		/* ease-out radius, linear fade */
		float t = (float)age / RIPPLE_MS;
		float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

		raster_ring(&r, ripples[i].x * RQ_ONE, (ripples[i].y) * RQ_ONE, (int)((24 + 70 * ease) * RQ_ONE),
			    2 * RQ_ONE, ripples[i].color, (uint8_t)(200 * (1.0f - t)));
	}

	for (int s = 0; s < TOUCH_MAX_POINTS; s++) {
		const struct finger *f = &fingers[s];
		uint32_t c = slot_color[s];

		/* trail, fading towards the tail */
		for (int k = 1; k < f->len; k++) {
			int a = (f->head + TRAIL - k) % TRAIL, b = (f->head + TRAIL - k - 1) % TRAIL;
			uint8_t alpha = (uint8_t)(220 * (f->len - k) / f->len);

			raster_line(&r, f->tx[a] * RQ_ONE, f->ty[a] * RQ_ONE, f->tx[b] * RQ_ONE, f->ty[b] * RQ_ONE,
				    c, 20, f->down ? alpha : alpha / 2);
		}
		if (!f->down) {
			continue;
		}
		int x = ts.pt[s].x, y = ts.pt[s].y;

		raster_fill_a(&r, r.box.x1, y, r.box.x2, y, c, 70);
		raster_fill_a(&r, x, r.box.y1, x, r.box.y2, c, 70);
		raster_ring(&r, x * RQ_ONE, y * RQ_ONE, 38 * RQ_ONE, 3 * RQ_ONE, c, 255);
		raster_disc(&r, x * RQ_ONE, y * RQ_ONE, 9 * RQ_ONE, c, 230);
	}

	/* a line between the first two contacts */
	int a = -1, b = -1;

	for (int s = 0; s < TOUCH_MAX_POINTS; s++) {
		if (fingers[s].down) {
			if (a < 0) {
				a = s;
			} else if (b < 0) {
				b = s;
			}
		}
	}
	if (b >= 0) {
		raster_line(&r, ts.pt[a].x * RQ_ONE, ts.pt[a].y * RQ_ONE, ts.pt[b].x * RQ_ONE,
			    ts.pt[b].y * RQ_ONE, C_TEXT, 10, 160);
	}
}

static void create(lv_obj_t *root)
{
	canvas = ui_box(root, 0, 0, GFX_W, UI_CONTENT_H);
	lv_obj_add_event_cb(canvas, canvas_draw, LV_EVENT_DRAW_MAIN, NULL);

	static const char *const caps[] = {"CONTACTS", "REPORT RATE", "TOUCH TO SWAP"};
	lv_obj_t **vals[] = {&l_contacts, &l_rate, &l_latency};

	for (int i = 0; i < 3; i++) {
		lv_obj_set_pos(ui_caption(root, caps[i]), UI_PAD + i * 150, 12);
		*vals[i] = ui_label(root, &font_textm_20, C_TEXT, "-");
		lv_obj_set_pos(*vals[i], UI_PAD + i * 150, 28);
	}
	l_hint = ui_label(root, &font_textm_20, C_TEXT2, "Touch anywhere. Up to five fingers.");
	lv_obj_align(l_hint, LV_ALIGN_CENTER, 0, 0);

	l_dist = ui_label(root, &font_mono_12, C_TEXT, "");
	lv_obj_add_flag(l_dist, LV_OBJ_FLAG_HIDDEN);
	for (int s = 0; s < TOUCH_MAX_POINTS; s++) {
		l_xy[s] = ui_label(root, &font_mono_12, slot_color[s], "");
		lv_obj_add_flag(l_xy[s], LV_OBJ_FLAG_HIDDEN);
	}
}

static void tick(uint32_t now_ms)
{
	static uint32_t rate_t0, rate_reports, text_t0;
	char buf[32];
	bool active = false;

	/* latency: a report seen last tick was in the frame that has now swapped */
	if (pending) {
		uint32_t d = gfx_last_present_us() - pending_report_us;

		if (d < 100000) {
			latency_us = latency_us ? (latency_us * 7 + d) / 8 : d;
		}
		pending = false;
	}

	uint32_t prev_reports = ts.reports;

	touch_get(&ts);
	if (ts.reports != prev_reports) {
		pending = true;
		pending_report_us = ts.last_us;
	}

	n_down = 0;
	for (int s = 0; s < TOUCH_MAX_POINTS; s++) {
		struct finger *f = &fingers[s];
		const struct touch_point *p = &ts.pt[s];
		bool down = p->down && !shell_gesture_active();

		if (down != f->down) {
			add_ripple(p->x, p->y, slot_color[s], now_ms);
			if (down) {
				f->len = 0;
			}
			f->down = down;
		}
		if (down) {
			n_down++;
			if (p->seq != f->seq) {
				f->seq = p->seq;
				f->head = (f->head + 1) % TRAIL;
				f->tx[f->head] = p->x;
				f->ty[f->head] = p->y;
				f->len = MIN(f->len + 1, TRAIL);
			}
			lv_label_set_text_fmt(l_xy[s], "%d  x %d  y %d", s + 1, p->x, p->y);
			lv_obj_set_pos(l_xy[s], CLAMP(p->x + 46, 4, GFX_W - 120), CLAMP(p->y - UI_CONTENT_Y - 44, 60, UI_CONTENT_H - 20));
			lv_obj_remove_flag(l_xy[s], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(l_xy[s], LV_OBJ_FLAG_HIDDEN);
			/* let the trail drain after release */
			if (f->len > 0) {
				f->len--;
				active = true;
			}
		}
	}
	if (n_down > 0) {
		last_activity_ms = now_ms;
		active = true;
	}
	for (int i = 0; i < RIPPLES; i++) {
		active |= ripples[i].on;
	}

	/* distance and angle between the first two contacts */
	int a = -1, b = -1;

	for (int s = 0; s < TOUCH_MAX_POINTS; s++) {
		if (fingers[s].down) {
			if (a < 0) {
				a = s;
			} else if (b < 0) {
				b = s;
			}
		}
	}
	if (b >= 0) {
		float dx = ts.pt[b].x - ts.pt[a].x, dy = ts.pt[b].y - ts.pt[a].y;

		snprintf(buf, sizeof(buf), "%.0f px  %.0f\xC2\xB0", (double)sqrtf(dx * dx + dy * dy),
			 (double)(atan2f(-dy, dx) * 57.29578f));
		lv_label_set_text(l_dist, buf);
		lv_obj_set_pos(l_dist, (ts.pt[a].x + ts.pt[b].x) / 2 + 10, (ts.pt[a].y + ts.pt[b].y) / 2 - UI_CONTENT_Y - 20);
		lv_obj_remove_flag(l_dist, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(l_dist, LV_OBJ_FLAG_HIDDEN);
	}

	if (now_ms - last_activity_ms > 2500) {
		lv_obj_remove_flag(l_hint, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(l_hint, LV_OBJ_FLAG_HIDDEN);
	}

	if (now_ms - rate_t0 >= 500) {
		rate_hz = (ts.reports - rate_reports) * 1000 / (now_ms - rate_t0);
		rate_reports = ts.reports;
		rate_t0 = now_ms;
	}
	if (now_ms - text_t0 >= 200) {
		text_t0 = now_ms;
		lv_label_set_text_fmt(l_contacts, "%d", n_down);
		if (n_down > 0) {
			lv_label_set_text_fmt(l_rate, "%u Hz", rate_hz);
			lv_label_set_text_fmt(l_latency, "%u.%u ms", latency_us / 1000, (latency_us / 100) % 10);
		}
	}
	if (active) {
		lv_obj_invalidate(canvas);
	}
}

static void mini(const struct raster *r)
{
	struct touch_state t;
	int w = lv_area_get_width(&r->box), h = lv_area_get_height(&r->box);
	bool any = false;

	touch_get(&t);
	for (int s = 0; s < TOUCH_MAX_POINTS; s++) {
		if (t.pt[s].down) {
			any = true;
			raster_disc(r, (r->box.x1 + t.pt[s].x * w / GFX_W) * RQ_ONE,
				    (r->box.y1 + t.pt[s].y * h / GFX_H) * RQ_ONE, 5 * RQ_ONE, slot_color[s], 255);
		}
	}
	if (!any) {
		/* idle: five dots on a slow Lissajous figure */
		float ph = k_uptime_get_32() * 0.0012f;

		for (int s = 0; s < TOUCH_MAX_POINTS; s++) {
			float a = ph + s * 0.55f;
			int x = (int)((r->box.x1 + w / 2 + sinf(a * 1.0f) * (w / 2 - 8)) * RQ_ONE);
			int y = (int)((r->box.y1 + h / 2 + sinf(a * 1.5f) * (h / 2 - 8)) * RQ_ONE);

			raster_disc(r, x, y, (5 - s / 2) * RQ_ONE, slot_color[s], 255 - s * 30);
		}
	}
}

const struct screen screen_touchlab = {
	.title = "Touch Lab",
	.blurb = "Five-point multi-touch",
	.create = create,
	.tick = tick,
	.mini = mini,
};
