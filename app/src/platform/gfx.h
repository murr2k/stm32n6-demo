/*
 * Display platform: frame buffers, LVGL display, vsync presentation, frame
 * timing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_GFX_H
#define PLATFORM_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

#define GFX_W        800
#define GFX_H        480
#define GFX_FB_BYTES (GFX_W * GFX_H * 4)

/* Frame history kept for the HUD and the Flight Deck. */
#define GFX_HISTORY 256

struct gfx_stats {
	uint32_t frames;        /* frames presented since boot */
	uint32_t dropped;       /* vsync intervals missed while animating */
	uint32_t refresh_uhz;   /* measured panel refresh, micro-hertz */
	uint32_t period_us;     /* nominal refresh period */
	uint32_t last_render_us; /* UI thread time for the frame: tick, layout and render */
	uint32_t last_period_us;
	uint32_t worst_render_us; /* since the last gfx_stats_reset_worst() */
	uint32_t worst_period_us;
	/* UI thread time after a present and before the next frame starts (LVGL
	 * timers that run after the display's, such as animation callbacks):
	 * not in render_us, but in the next frame's budget. Since the reset. */
	uint32_t worst_tail_us;
	uint32_t fps_x10;      /* presented frames per second, 1 s window */
	uint32_t avg_render_us; /* 1 s window */
	uint16_t head;          /* next write index in the rings below */
	uint16_t render_us[GFX_HISTORY]; /* saturated at 65535 */
	uint16_t period_us_ring[GFX_HISTORY];
};

int gfx_init(void);

/* Run LVGL once: timers, animations, render, present. Blocks until the swap
 * when something was drawn, otherwise sleeps to the next frame slot.
 */
void gfx_run_frame(void);

const struct gfx_stats *gfx_stats(void);
void gfx_stats_reset_worst(void);
lv_display_t *gfx_display(void);

/* Copy the frame currently on the panel into a stable buffer at the next frame
 * boundary and return it (ARGB8888, GFX_FB_BYTES). Called from other threads.
 */
const uint8_t *gfx_snapshot(void);

/* Fade the whole display towards the background colour in hardware: the LTDC
 * layer's constant alpha, 255 = fully visible. Costs no CPU time, unlike
 * blending a veil over the frame buffer. Takes effect at the next vertical blank.
 */
void gfx_set_fade(uint8_t alpha);

/* Time of the most recent buffer swap, gfx_now_us() timebase. */
uint32_t gfx_last_present_us(void);

/* Microseconds since boot, from the 800 MHz cycle counter. */
uint32_t gfx_now_us(void);

/*
 * Lending frame buffer B (GFX_FB_B, 1.5 MB in the SRAM banks beside the NPU)
 * to the NPU while a recognition view is on screen. The display then runs
 * single-buffered from frame buffer A, rendering from the top of each refresh
 * (the camera picture, drawn first and by DMA2D, stays ahead of the scanout).
 *
 * gfx_lend_back_buffer(true) and (false) are called by a screen's show() and
 * hide() (UI thread, under the transition fade). The switch completes over
 * the next frames: lending once frame buffer A is on the panel, returning
 * once the borrower has let go. The borrower brackets every use with
 * gfx_borrow_begin(), which fails while the buffer is not lent, and
 * gfx_borrow_end().
 */
#define GFX_FB_B 0x34249000u

void gfx_lend_back_buffer(bool lend);
bool gfx_borrow_begin(void);
void gfx_borrow_end(void);
bool gfx_back_buffer_lent(void);

#endif
