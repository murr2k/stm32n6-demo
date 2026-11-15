/*
 * Direct rasteriser. See raster.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/sys/util.h>

#include <lvgl_private.h> /* lv_layer_t internals, lv_area_intersect() */

#include "raster.h"

bool raster_begin(struct raster *r, lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_obj_t *obj = lv_event_get_current_target(e);
	lv_draw_buf_t *db = layer ? layer->draw_buf : NULL;
	lv_area_t tmp;

	if (db == NULL || (layer->color_format != LV_COLOR_FORMAT_XRGB8888 &&
			   layer->color_format != LV_COLOR_FORMAT_ARGB8888)) {
		return false;
	}
	lv_obj_get_coords(obj, &r->box);
	if (!lv_area_intersect(&tmp, &r->box, &layer->_clip_area) ||
	    !lv_area_intersect(&r->clip, &tmp, &layer->buf_area)) {
		return false;
	}
	r->stride = db->header.stride / 4;
	r->base = (uint32_t *)db->data - layer->buf_area.y1 * r->stride - layer->buf_area.x1;
	return true;
}

static inline uint32_t *px(const struct raster *r, int x, int y)
{
	return r->base + y * r->stride + x;
}

void raster_fill(const struct raster *r, int x1, int y1, int x2, int y2, uint32_t rgb)
{
	x1 = MAX(x1, r->clip.x1);
	y1 = MAX(y1, r->clip.y1);
	x2 = MIN(x2, r->clip.x2);
	y2 = MIN(y2, r->clip.y2);
	if (x1 > x2 || y1 > y2) {
		return;
	}
	rgb |= 0xff000000u;
	for (int y = y1; y <= y2; y++) {
		uint32_t *p = px(r, x1, y);

		for (int n = x2 - x1 + 1; n > 0; n--) {
			*p++ = rgb;
		}
	}
}

void raster_fill_a(const struct raster *r, int x1, int y1, int x2, int y2, uint32_t rgb, uint8_t a)
{
	if (a == 255) {
		raster_fill(r, x1, y1, x2, y2, rgb);
		return;
	}
	x1 = MAX(x1, r->clip.x1);
	y1 = MAX(y1, r->clip.y1);
	x2 = MIN(x2, r->clip.x2);
	y2 = MIN(y2, r->clip.y2);
	if (x1 > x2 || y1 > y2 || a == 0) {
		return;
	}
	for (int y = y1; y <= y2; y++) {
		uint32_t *p = px(r, x1, y);

		for (int n = x2 - x1 + 1; n > 0; n--, p++) {
			*p = raster_blend(*p, rgb, a);
		}
	}
}

void raster_hline_dash(const struct raster *r, int x1, int x2, int y, uint32_t rgb, uint8_t a, int on,
		       int off)
{
	if (y < r->clip.y1 || y > r->clip.y2) {
		return;
	}
	for (int x = x1; x <= x2; x += on + off) {
		raster_fill_a(r, x, y, MIN(x + on - 1, x2), y, rgb, a);
	}
}

/* One pixel column covering [top_q, bot_q) with fractional coverage at the ends. */
static void vspan(const struct raster *r, int x, int top_q, int bot_q, uint32_t rgb, uint32_t a)
{
	if (x < r->clip.x1 || x > r->clip.x2 || bot_q <= top_q) {
		return;
	}
	int y1 = top_q >> RQ;
	int y2 = (bot_q - 1) >> RQ;

	if (y2 < r->clip.y1 || y1 > r->clip.y2) {
		return;
	}
	if (y1 == y2) {
		if (y1 >= r->clip.y1) {
			uint32_t *p = px(r, x, y1);

			*p = raster_blend(*p, rgb, (a * (bot_q - top_q)) >> RQ);
		}
		return;
	}
	uint32_t a_top = (a * (RQ_ONE - (top_q & (RQ_ONE - 1)))) >> RQ;
	uint32_t a_bot = (a * (((bot_q - 1) & (RQ_ONE - 1)) + 1)) >> RQ;

	if (y1 >= r->clip.y1) {
		uint32_t *p = px(r, x, y1);

		*p = raster_blend(*p, rgb, a_top);
	}
	if (y2 <= r->clip.y2) {
		uint32_t *p = px(r, x, y2);

		*p = raster_blend(*p, rgb, a_bot);
	}
	int ys = MAX(y1 + 1, r->clip.y1);
	int ye = MIN(y2 - 1, r->clip.y2);
	uint32_t *p = px(r, x, ys);

	if (a >= 255) {
		for (int y = ys; y <= ye; y++, p += r->stride) {
			*p = rgb | 0xff000000u;
		}
	} else {
		for (int y = ys; y <= ye; y++, p += r->stride) {
			*p = raster_blend(*p, rgb, a);
		}
	}
}

void raster_trace(const struct raster *r, int x0, const int32_t *y_q, int n, uint32_t rgb, int half_w,
		  uint8_t a)
{
	int i0 = MAX(0, r->clip.x1 - x0);
	int i1 = MIN(n - 1, r->clip.x2 - x0);

	for (int i = i0; i <= i1; i++) {
		int32_t lo = y_q[i], hi = y_q[i];

		/* Join to the neighbours' midpoints so steep segments stay connected. */
		if (i > 0) {
			int32_t m = (y_q[i - 1] + y_q[i]) / 2;

			lo = MIN(lo, m);
			hi = MAX(hi, m);
		}
		if (i < n - 1) {
			int32_t m = (y_q[i + 1] + y_q[i]) / 2;

			lo = MIN(lo, m);
			hi = MAX(hi, m);
		}
		vspan(r, x0 + i, lo - half_w, hi + half_w, rgb, a);
	}
}

void raster_envelope(const struct raster *r, int x0, const int32_t *lo_q, const int32_t *hi_q, int n,
		     uint32_t rgb, int half_w, uint8_t a)
{
	int i0 = MAX(0, r->clip.x1 - x0);
	int i1 = MIN(n - 1, r->clip.x2 - x0);

	for (int i = i0; i <= i1; i++) {
		int32_t lo = lo_q[i], hi = hi_q[i];

		/* close the gap to a neighbour whose range does not overlap this one */
		if (i > 0) {
			if (hi_q[i - 1] < lo) {
				lo = (hi_q[i - 1] + lo) / 2;
			} else if (lo_q[i - 1] > hi) {
				hi = (lo_q[i - 1] + hi) / 2;
			}
		}
		if (i < n - 1) {
			if (hi_q[i + 1] < lo) {
				lo = (hi_q[i + 1] + lo) / 2;
			} else if (lo_q[i + 1] > hi) {
				hi = (lo_q[i + 1] + hi) / 2;
			}
		}
		vspan(r, x0 + i, lo - half_w, hi + half_w, rgb, a);
	}
}

void raster_area(const struct raster *r, int x0, const int32_t *y_q, int n, int y_top, int y_base,
		 uint32_t rgb, uint8_t a_top)
{
	int i0 = MAX(0, r->clip.x1 - x0);
	int i1 = MIN(n - 1, r->clip.x2 - x0);
	int span = MAX(y_base - y_top, 1);
	int ye = MIN(y_base, r->clip.y2);

	for (int i = i0; i <= i1; i++) {
		int ys = MAX((y_q[i] >> RQ) + 1, r->clip.y1);
		uint32_t *p = px(r, x0 + i, ys);

		for (int y = ys; y <= ye; y++, p += r->stride) {
			uint32_t a = (uint32_t)a_top * (y_base - y) / span;

			*p = raster_blend(*p, rgb, MIN(a, 255u));
		}
	}
}

void raster_lut8(const struct raster *r, int x, int y, int w, int h, const uint8_t *src,
		 int src_stride, const uint32_t *lut)
{
	int x1 = MAX(x, r->clip.x1), x2 = MIN(x + w - 1, r->clip.x2);
	int y1 = MAX(y, r->clip.y1), y2 = MIN(y + h - 1, r->clip.y2);

	for (int yy = y1; yy <= y2; yy++) {
		const uint8_t *s = src + (yy - y) * src_stride + (x1 - x);
		uint32_t *p = px(r, x1, yy);

		for (int n = x2 - x1 + 1; n > 0; n--) {
			*p++ = lut[*s++];
		}
	}
}

/* Integer square root of a 32-bit value. */
static uint32_t isqrt(uint32_t v)
{
	uint32_t res = 0, bit = 1u << 30;

	while (bit > v) {
		bit >>= 2;
	}
	while (bit) {
		if (v >= res + bit) {
			v -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}
	return res;
}

/* Coverage of a disc edge: signed distance (1/16 px) inside the edge, to 0..255. */
static inline uint32_t edge_cov(int d_q)
{
	if (d_q >= RQ_ONE / 2) {
		return 255;
	}
	if (d_q <= -RQ_ONE / 2) {
		return 0;
	}
	return (uint32_t)((d_q + RQ_ONE / 2) * 255) >> RQ;
}

void raster_ring(const struct raster *r, int cx_q, int cy_q, int radius_q, int width_q, uint32_t rgb,
		 uint8_t a)
{
	int x1 = MAX((cx_q - radius_q) >> RQ, r->clip.x1);
	int x2 = MIN(((cx_q + radius_q) >> RQ) + 1, r->clip.x2);
	int y1 = MAX((cy_q - radius_q) >> RQ, r->clip.y1);
	int y2 = MIN(((cy_q + radius_q) >> RQ) + 1, r->clip.y2);
	int inner_q = radius_q - width_q;

	for (int y = y1; y <= y2; y++) {
		int dy = (y << RQ) + RQ_ONE / 2 - cy_q;
		uint32_t *p = px(r, x1, y);

		for (int x = x1; x <= x2; x++, p++) {
			int dx = (x << RQ) + RQ_ONE / 2 - cx_q;
			int d = (int)isqrt((uint32_t)(dx * dx + dy * dy));
			uint32_t c = edge_cov(radius_q - d);

			if (width_q > 0) {
				c = c * edge_cov(d - inner_q) / 255;
			}
			if (c) {
				*p = raster_blend(*p, rgb, c * a / 255);
			}
		}
	}
}

void raster_disc(const struct raster *r, int cx_q, int cy_q, int radius_q, uint32_t rgb, uint8_t a)
{
	raster_ring(r, cx_q, cy_q, radius_q, 0, rgb, a);
}

void raster_line(const struct raster *r, int x0_q, int y0_q, int x1_q, int y1_q, uint32_t rgb,
		 int half_w, uint8_t a)
{
	int dx = x1_q - x0_q, dy = y1_q - y0_q;

	if (LV_ABS(dx) >= LV_ABS(dy)) {
		/* mostly horizontal: one vertical span per column */
		if (dx < 0) {
			int t;

			t = x0_q, x0_q = x1_q, x1_q = t;
			t = y0_q, y0_q = y1_q, y1_q = t;
			dx = -dx;
			dy = -dy;
		}
		int xs = MAX(x0_q >> RQ, r->clip.x1), xe = MIN(x1_q >> RQ, r->clip.x2);

		for (int x = xs; x <= xe; x++) {
			int y = dx ? y0_q + (int)((int64_t)dy * ((x << RQ) + RQ_ONE / 2 - x0_q) / dx) : y0_q;

			vspan(r, x, y - half_w, y + half_w, rgb, a);
		}
	} else {
		/* mostly vertical: horizontal coverage per row */
		if (dy < 0) {
			int t;

			t = x0_q, x0_q = x1_q, x1_q = t;
			t = y0_q, y0_q = y1_q, y1_q = t;
			dx = -dx;
			dy = -dy;
		}
		int ys = MAX(y0_q >> RQ, r->clip.y1), ye = MIN(y1_q >> RQ, r->clip.y2);

		for (int y = ys; y <= ye; y++) {
			int x = x0_q + (int)((int64_t)dx * ((y << RQ) + RQ_ONE / 2 - y0_q) / dy);
			int l = x - half_w, rr = x + half_w;
			int xa = l >> RQ, xb = (rr - 1) >> RQ;

			for (int xx = MAX(xa, r->clip.x1); xx <= MIN(xb, r->clip.x2); xx++) {
				int cov = MIN(rr, (xx + 1) << RQ) - MAX(l, xx << RQ);
				uint32_t *p = px(r, xx, y);

				*p = raster_blend(*p, rgb, (uint32_t)a * cov >> RQ);
			}
		}
	}
}
