/*
 * Direct rasteriser for dense data visualisation.
 *
 * LVGL's draw primitives create one task per line segment or rectangle, which
 * is far too slow for a trace with hundreds of points per frame. These
 * routines write straight into the layer buffer from inside a widget's
 * LV_EVENT_DRAW_MAIN handler. That is safe because the software renderer runs
 * without an OS here, so every earlier draw task has already executed by the
 * time the handler runs.
 *
 * All coordinates are absolute screen pixels. Everything is clipped.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef UI_RASTER_H
#define UI_RASTER_H

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

struct raster {
	uint32_t *base;   /* address of pixel (0, 0) in screen coordinates */
	int32_t stride;   /* pixels per row */
	lv_area_t clip;   /* inclusive, screen coordinates */
	lv_area_t box;    /* the widget's own coordinates */
};

/* Sub-pixel y: 4 fractional bits. */
#define RQ      4
#define RQ_ONE  (1 << RQ)

/* Returns false when nothing of the widget is inside the redraw area. */
bool raster_begin(struct raster *r, lv_event_t *e);

void raster_fill(const struct raster *r, int x1, int y1, int x2, int y2, uint32_t rgb);
void raster_fill_a(const struct raster *r, int x1, int y1, int x2, int y2, uint32_t rgb, uint8_t a);
void raster_hline_dash(const struct raster *r, int x1, int x2, int y, uint32_t rgb, uint8_t a,
		       int on, int off);

/* Anti-aliased trace through n samples, one per pixel column starting at x0.
 * y values are screen coordinates in 1/16 px. half_w is half the stroke width
 * in 1/16 px (12 gives a 1.5 px line).
 */
void raster_trace(const struct raster *r, int x0, const int32_t *y_q, int n, uint32_t rgb,
		  int half_w, uint8_t a);

/* Min/max envelope: one vertical span per column from lo_q to hi_q (1/16 px,
 * lo above hi on screen, so lo <= hi numerically). This is how a million
 * samples are drawn in 768 columns. Columns are joined to their neighbours.
 */
void raster_envelope(const struct raster *r, int x0, const int32_t *lo_q, const int32_t *hi_q, int n,
		     uint32_t rgb, int half_w, uint8_t a);

/* An envelope with a translucent body (opacity body_a) and opaque edges, the
 * same pixels as raster_envelope() followed by raster_trace() on lo_q and on
 * hi_q, but column by column: each column's cache lines are visited once
 * instead of three times, which is where the time goes on a noisy signal.
 */
void raster_band(const struct raster *r, int x0, const int32_t *lo_q, const int32_t *hi_q, int n,
		 uint32_t rgb, int body_half_w, uint8_t body_a, int edge_half_w);

/* Gradient fill between the trace and y_base: opacity a_top at y_top fading to
 * zero at y_base.
 */
void raster_area(const struct raster *r, int x0, const int32_t *y_q, int n, int y_top, int y_base,
		 uint32_t rgb, uint8_t a_top);

/* Blit an 8-bit map through a 256-entry colour table (heat maps, waterfalls). */
void raster_lut8(const struct raster *r, int x, int y, int w, int h, const uint8_t *src,
		 int src_stride, const uint32_t *lut);

/* Anti-aliased filled disc and ring, centre and radius in 1/16 px. */
void raster_disc(const struct raster *r, int cx_q, int cy_q, int radius_q, uint32_t rgb, uint8_t a);
void raster_ring(const struct raster *r, int cx_q, int cy_q, int radius_q, int width_q, uint32_t rgb,
		 uint8_t a);

/* Anti-aliased line of arbitrary direction, endpoints in 1/16 px. */
void raster_line(const struct raster *r, int x0_q, int y0_q, int x1_q, int y1_q, uint32_t rgb,
		 int half_w, uint8_t a);

static inline uint32_t raster_blend(uint32_t dst, uint32_t rgb, uint32_t a)
{
	uint32_t ia = 256 - (a += a >> 7);
	uint32_t rb = (((rgb & 0xff00ff) * a + (dst & 0xff00ff) * ia) >> 8) & 0xff00ff;
	uint32_t g = (((rgb & 0x00ff00) * a + (dst & 0x00ff00) * ia) >> 8) & 0x00ff00;

	return 0xff000000u | rb | g;
}

#endif
