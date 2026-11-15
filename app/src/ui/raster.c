/*
 * Direct rasteriser. See raster.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>

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

/* Column i of a trace: the sample, joined to the neighbours' midpoints so steep
 * segments stay connected.
 */
static inline void trace_col(const int32_t *y_q, int i, int n, int32_t *lo_out, int32_t *hi_out)
{
	int32_t lo = y_q[i], hi = y_q[i];

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
	*lo_out = lo;
	*hi_out = hi;
}

/* Column i of an envelope: its range, extended to close the gap to a
 * neighbour whose range does not overlap this one.
 */
static inline void envelope_col(const int32_t *lo_q, const int32_t *hi_q, int i, int n, int32_t *lo_out,
				int32_t *hi_out)
{
	int32_t lo = lo_q[i], hi = hi_q[i];

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
	*lo_out = lo;
	*hi_out = hi;
}

void raster_trace(const struct raster *r, int x0, const int32_t *y_q, int n, uint32_t rgb, int half_w,
		  uint8_t a)
{
	int i0 = MAX(0, r->clip.x1 - x0);
	int i1 = MIN(n - 1, r->clip.x2 - x0);

	for (int i = i0; i <= i1; i++) {
		int32_t lo, hi;

		trace_col(y_q, i, n, &lo, &hi);
		vspan(r, x0 + i, lo - half_w, hi + half_w, rgb, a);
	}
}

void raster_envelope(const struct raster *r, int x0, const int32_t *lo_q, const int32_t *hi_q, int n,
		     uint32_t rgb, int half_w, uint8_t a)
{
	int i0 = MAX(0, r->clip.x1 - x0);
	int i1 = MIN(n - 1, r->clip.x2 - x0);

	for (int i = i0; i <= i1; i++) {
		int32_t lo, hi;

		envelope_col(lo_q, hi_q, i, n, &lo, &hi);
		vspan(r, x0 + i, lo - half_w, hi + half_w, rgb, a);
	}
}

void raster_band(const struct raster *r, int x0, const int32_t *lo_q, const int32_t *hi_q, int n,
		 uint32_t rgb, int body_half_w, uint8_t body_a, int edge_half_w)
{
	int i0 = MAX(0, r->clip.x1 - x0);
	int i1 = MIN(n - 1, r->clip.x2 - x0);

	for (int i = i0; i <= i1; i++) {
		int32_t lo, hi;

		envelope_col(lo_q, hi_q, i, n, &lo, &hi);
		vspan(r, x0 + i, lo - body_half_w, hi + body_half_w, rgb, body_a);
		trace_col(lo_q, i, n, &lo, &hi);
		vspan(r, x0 + i, lo - edge_half_w, hi + edge_half_w, rgb, 255);
		trace_col(hi_q, i, n, &lo, &hi);
		vspan(r, x0 + i, lo - edge_half_w, hi + edge_half_w, rgb, 255);
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
	const int half = RQ_ONE / 2;

	/* Squared-distance bounds that settle a pixel without a square root: only
	 * the pixels on an edge need the distance itself. Each bound is the
	 * conservative side of the edge_cov() thresholds, so the result is the same.
	 */
	uint32_t outside2 = (uint32_t)((radius_q + half) * (radius_q + half));
	uint32_t full2 = radius_q > half ? (uint32_t)((radius_q - half) * (radius_q - half)) : 0;
	bool ring = width_q > 0;
	uint32_t hole2 = ring && inner_q > half ? (uint32_t)((inner_q - half) * (inner_q - half)) : 0;
	uint32_t solid2 = ring && inner_q + half > 0 ? (uint32_t)((inner_q + half) * (inner_q + half)) : 0;

	for (int y = y1; y <= y2; y++) {
		int dy = (y << RQ) + RQ_ONE / 2 - cy_q;
		uint32_t *p = px(r, x1, y);

		for (int x = x1; x <= x2; x++, p++) {
			int dx = (x << RQ) + RQ_ONE / 2 - cx_q;
			uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
			uint32_t c;

			if (d2 >= outside2 || (hole2 && d2 <= hole2)) {
				continue;
			}
			if (d2 <= full2 && d2 >= solid2 && full2) {
				c = 255;
			} else {
				int d = (int)isqrt(d2);

				c = edge_cov(radius_q - d);
				if (ring) {
					c = c * edge_cov(d - inner_q) / 255;
				}
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
	int major = MAX(LV_ABS(dx), LV_ABS(dy));

	if (major == 0) {
		return;
	}
	/* The stroke is measured along the minor axis, so widen it by 1 / cos of
	 * the slope: otherwise a diagonal is 30 % thinner than a level line and a
	 * rotating wireframe changes brightness as it turns.
	 */
	half_w = (int)((float)half_w * sqrtf((float)dx * dx + (float)dy * dy) / major + 0.5f);

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
		float slope = (float)dy / dx; /* not a 64-bit division per column: that was most of the cost */

		for (int x = xs; x <= xe; x++) {
			int y = y0_q + (int)(slope * (float)((x << RQ) + RQ_ONE / 2 - x0_q));

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
		float slope = (float)dx / dy;

		for (int y = ys; y <= ye; y++) {
			int x = x0_q + (int)(slope * (float)((y << RQ) + RQ_ONE / 2 - y0_q));
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

/* One side of a convex polygon, walked downwards from the top vertex. */
struct poly_chain {
	int i;    /* vertex the current edge starts at */
	int step; /* 1 or n - 1: which way round the polygon */
	int left; /* edges still to go, including the current one */
	float xa, ya, yb, slope;
};

static inline void chain_load(struct poly_chain *c, const float *x, const float *y, int n)
{
	int j = (c->i + c->step) % n;
	float dy = y[j] - y[c->i];

	c->xa = x[c->i];
	c->ya = y[c->i];
	c->yb = y[j];
	c->slope = dy > 1e-4f ? (x[j] - x[c->i]) / dy : 0.0f;
}

/* x of the chain at height t. Every vertex passed on the way widens [lo, hi]:
 * a corner inside a pixel row can stick out further than the edge does at the
 * top or the bottom of the row.
 */
static inline float chain_at(struct poly_chain *c, const float *x, const float *y, int n, float t, float *lo,
			     float *hi)
{
	while (t > c->yb && c->left > 1) {
		c->i = (c->i + c->step) % n;
		c->left--;
		*lo = MIN(*lo, x[c->i]);
		*hi = MAX(*hi, x[c->i]);
		chain_load(c, x, y, n);
	}
	return c->xa + (CLAMP(t, c->ya, c->yb) - c->ya) * c->slope;
}

static inline int ifloor(float v)
{
	int i = (int)v;

	return i - (v < (float)i);
}

static inline int iceil(float v)
{
	int i = (int)v;

	return i + (v > (float)i);
}

void raster_poly(const struct raster *r, const float *x, const float *y, int n, uint32_t rgb, uint8_t a)
{
	float xmin = x[0], xmax = x[0];
	int top = 0, bot = 0;

	if (n < 3 || n > RASTER_POLY_MAX || a == 0) {
		return;
	}
	for (int i = 1; i < n; i++) {
		if (y[i] < y[top]) {
			top = i;
		}
		if (y[i] > y[bot]) {
			bot = i;
		}
		xmin = MIN(xmin, x[i]);
		xmax = MAX(xmax, x[i]);
	}
	float ymin = y[top], ymax = y[bot];

	if (xmax < r->clip.x1 || xmin > r->clip.x2 + 1 || ymax < r->clip.y1 || ymin > r->clip.y2 + 1 ||
	    !(xmax - xmin < 1e6f) || !(ymax - ymin < 1e6f) || !(ymax - ymin > 1e-3f)) {
		return;
	}
	/* the two sides, from the top vertex down to the bottom one */
	struct poly_chain c1 = {.i = top, .step = 1, .left = (bot - top + n) % n};
	struct poly_chain c2 = {.i = top, .step = n - 1, .left = (top - bot + n) % n};
	struct poly_chain *both[2] = {&c1, &c2};

	for (int k = 0; k < 2; k++) {
		struct poly_chain *c = both[k];

		chain_load(c, x, y, n);
		while (c->left > 1 && c->yb - c->ya <= 1e-4f) { /* a level top edge */
			c->i = (c->i + c->step) % n;
			c->left--;
			chain_load(c, x, y, n);
		}
	}
	int y0 = MAX(ifloor(ymin), r->clip.y1);
	int y1 = MIN(iceil(ymax) - 1, r->clip.y2);
	float lo = 1e9f, hi = -1e9f;
	float ta = MAX((float)y0, ymin);
	float a1 = chain_at(&c1, x, y, n, ta, &lo, &hi), a2 = chain_at(&c2, x, y, n, ta, &lo, &hi);

	for (int yy = y0; yy <= y1; yy++) {
		/* the part of this pixel row that the polygon spans: [ta, tb] */
		float tb = MIN((float)(yy + 1), ymax);

		lo = 1e9f, hi = -1e9f;
		float b1 = chain_at(&c1, x, y, n, tb, &lo, &hi), b2 = chain_at(&c2, x, y, n, tb, &lo, &hi);
		/* each side sweeps from its x at the top of the row to its x at the bottom */
		float la = MIN(a1, a2), ra = MAX(a1, a2), lb = MIN(b1, b2), rb = MAX(b1, b2);
		float l_lo = MIN(MIN(la, lb), lo), l_hi = MAX(la, lb), r_lo = MIN(ra, rb), r_hi = MAX(MAX(ra, rb), hi);
		float span = tb - ta;

		a1 = b1, a2 = b2, ta = tb;
		if (span <= 0.0f) {
			continue;
		}
		/* coverage ramps: at least a pixel wide, as wide as the edge's sweep */
		float wl = MAX(l_hi - l_lo, 1.0f), ml = 0.5f * (l_lo + l_hi);
		float wr = MAX(r_hi - r_lo, 1.0f), mr = 0.5f * (r_lo + r_hi);
		float kl = wl > 1.0f ? 1.0f / wl : 1.0f, kr = wr > 1.0f ? 1.0f / wr : 1.0f;
		float row_a = (float)a * span;
		int xs = MAX(ifloor(ml - 0.5f * wl), r->clip.x1);
		int xe = MIN(iceil(mr + 0.5f * wr) - 1, r->clip.x2);
		int in0 = iceil(ml + 0.5f * wl - 0.5f);  /* first pixel fully inside the left edge */
		int in1 = ifloor(mr - 0.5f * wr - 0.5f); /* last pixel fully inside the right edge */

		if (xs > xe) {
			continue;
		}
		uint32_t *p = px(r, xs, yy);
		int xx = xs;

		if (in0 <= in1) {
			int e0 = MIN(in0, xe + 1), e1 = MIN(in1, xe);

			for (; xx < e0; xx++, p++) {
				float c = ((float)xx + 0.5f - ml) * kl + 0.5f;

				if (c > 0.0f) {
					*p = raster_blend(*p, rgb, (uint32_t)(row_a * MIN(c, 1.0f)));
				}
			}
			if (row_a >= 254.5f) {
				uint32_t solid = rgb | 0xff000000u;

				for (; xx <= e1; xx++) {
					*p++ = solid;
				}
			} else {
				uint32_t ai = (uint32_t)row_a;

				for (; xx <= e1; xx++, p++) {
					*p = raster_blend(*p, rgb, ai);
				}
			}
			for (; xx <= xe; xx++, p++) {
				float c = (mr - ((float)xx + 0.5f)) * kr + 0.5f;

				if (c > 0.0f) {
					*p = raster_blend(*p, rgb, (uint32_t)(row_a * MIN(c, 1.0f)));
				}
			}
		} else {
			/* a sliver: the two ramps overlap */
			for (; xx <= xe; xx++, p++) {
				float cl = CLAMP(((float)xx + 0.5f - ml) * kl + 0.5f, 0.0f, 1.0f);
				float cr = CLAMP((mr - ((float)xx + 0.5f)) * kr + 0.5f, 0.0f, 1.0f);
				float c = cl + cr - 1.0f;

				if (c > 0.0f) {
					*p = raster_blend(*p, rgb, (uint32_t)(row_a * c));
				}
			}
		}
	}
}