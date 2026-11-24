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
	/* The least time left before the swap deadline (the end of the blanking
	 * before the refresh that should show it, gfx.c) when a frame was handed
	 * over, since the reset; negative: a frame missed it. */
	int32_t margin_min_us;
	/* frames handed over after the vertical sync began but still swapped in
	 * the blanking: with a swap at line 0 each would have been a dropped frame */
	uint32_t late_swaps;
	/* frames that waited for a reader (the recorder) to finish with the
	 * buffer they draw into, and the longest wait */
	uint32_t reader_waits;
	uint32_t reader_wait_worst_us;
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

/* The next `frames` frames may drop because the caller does something costly on
 * purpose (a test command): their drops are counted as always, but they do not
 * trigger zprof's flight recorder (ZPROF_TRIGGER). UI thread only. */
void gfx_expect_drop(int frames);
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

/* Show the whole picture `rows` lines lower, in hardware (the layer's window
 * starts lower; the background colour fills the lines above it), 0 = in
 * place. For a transition's settle: moving the screen's objects instead
 * makes LVGL redraw all of them on every frame of it. Like the fade, it costs
 * no render time and takes effect at the next vertical blank. */
void gfx_set_offset(int rows);

/*
 * Where the time went, frame by frame: through a screen transition (ctl
 * "swtrace") and before the last dropped frame (ctl "droptrace"). A frame is
 * on time if it is handed over before the next swap (the line interrupt at
 * line 0); its budget from the previous swap holds the wake-up after that
 * swap, that present's tail (a screen switch runs there), the main loop's
 * work before LVGL and LVGL's own.
 */
#define GFX_TRACE_N 26 /* fade-out, switch, hold and fade-in */
#define GFX_DROP_N  8

struct gfx_trace_frame {
	uint16_t wake_us; /* the previous swap to the UI thread running again */
	uint16_t tail_us; /* the previous present to the end of its LVGL pass */
	uint16_t pre_us;  /* main loop before LVGL: control, telemetry, screen ticks */
	uint16_t lv_us;   /* LVGL: timers, layout, render, up to the hand-over */
	uint16_t away_us; /* of tail, pre and lv: time the UI thread was not running */
	int16_t left_us;  /* the next swap minus the hand-over; negative: missed */
	uint8_t alpha;    /* the layer's fade when the frame was handed over */
	bool locked;      /* on a deadline: the previous frame was presented (gfx.c margin_skip: two frames entering the Camera screen are not) */
};

struct gfx_trace {
	uint32_t seq;       /* transitions traced since boot */
	int8_t from, to;
	uint8_t n;          /* frames recorded so far, up to GFX_TRACE_N */
	uint8_t at_switch;  /* f[at_switch] is the first frame after the switch (n if not yet) */
	uint16_t switch_us; /* the switch itself (hide, show, attract start), in that frame's tail */
	struct gfx_trace_frame f[GFX_TRACE_N];
};

struct gfx_drop {
	uint32_t frame;        /* stats.frames when it happened (0: none yet) */
	uint32_t seq;          /* the transition trace's seq at the time */
	int8_t screen;         /* on screen, or being switched to */
	struct gfx_trace_frame f[GFX_DROP_N]; /* oldest first, the late frame last */
};

/* The shell (UI thread): a transition starts (the fade-out), and the switch
 * inside it is made. */
void gfx_trace_begin(int from, int to);
void gfx_trace_switch(int to, uint32_t switch_us);
const struct gfx_trace *gfx_trace(void);
const struct gfx_drop *gfx_last_drop(void);

/*
 * Frame margins: the time left at each hand-over (left_us above) of every frame
 * on a deadline, in bins, and the least left by each screen's first frame after
 * a switch into it (the frame that builds it; the switch runs in its tail) and
 * by the frames after that one while the screen fades in (they draw what the
 * first one left out, shell_entering()). Since boot or gfx_margins_reset()
 * (ctl "margins"). A near miss left less than
 * GFX_NEAR_PCT % of the refresh (misses included): a reporting line, the
 * headroom that frames' jitter has needed (2.7 ms on one screen's first frame
 * in the 2026-11-23 hunt), not a rule the UI applies.
 */
#define GFX_MARGIN_BIN_US 500
#define GFX_MARGIN_BINS   34 /* [0] missed; [k] (k - 1) to k half-milliseconds left; [33] 16 ms or more */
#define GFX_NEAR_PCT      20
#define GFX_SCREENS       16 /* the shell's MAX_SCREENS */

struct gfx_margins {
	uint32_t frames;                   /* on a deadline */
	uint32_t near;                     /* near misses, misses included */
	uint32_t hist[GFX_MARGIN_BINS];
	int32_t first_min_us[GFX_SCREENS]; /* valid where first_n > 0 */
	uint16_t first_n[GFX_SCREENS];
	int32_t next_min_us[GFX_SCREENS];  /* the frames after it, under the fade; valid where next_n > 0 */
	uint16_t next_n[GFX_SCREENS];
};

const struct gfx_margins *gfx_margins(void);
void gfx_margins_reset(void);
uint32_t gfx_near_floor_us(void);

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
/* Called on the UI thread, before LVGL runs, when this frame will draw the
 * whole screen for the display's own reasons (lending B begins, or B comes
 * back with whatever the NPU left in it, at any frame after the switch): the
 * shell holds back the costly content as for a screen's first frame. */
void gfx_on_full_redraw(void (*cb)(void));
bool gfx_borrow_begin(void);
void gfx_borrow_end(void);
bool gfx_back_buffer_lent(void);

/*
 * Reading what is on screen with a bus master (the recorder's encoder).
 * gfx_sleep_to_active() returns just after the next active area of the panel
 * began (its time, gfx_now_us()): the buffer on screen, gfx_front(), stays on
 * screen until the next blanking. Between gfx_reader_begin(buf) and
 * gfx_reader_end() the UI thread does not start a frame that draws into buf:
 * it waits (at most 30 ms) and counts the wait in the stats. In double
 * buffering that buffer is drawn into only after the next present, so a
 * reader that is done within a refresh never makes it wait; single-buffered
 * (gfx_single_buffered(), the Camera screen) the display draws into the
 * buffer it shows, and a picture read meanwhile may tear.
 */
uint32_t gfx_sleep_to_active(void);
const uint8_t *gfx_front(void);
bool gfx_single_buffered(void);
void gfx_reader_begin(const uint8_t *buf);
void gfx_reader_end(void);

/*
 * A copy of the picture on screen into dst (GFX_FB_BYTES, not read by the
 * CPU: a bus master's input), made by DMA2D on the UI thread at the end of
 * its next frame. Single-buffered that is between the frame's end and the
 * next refresh, where the UI thread would otherwise wait: the display does
 * not wait for the reader, and the copy is a whole frame, never a half-drawn
 * one. Blocks the caller; returns the time the copy began (gfx_now_us(),
 * never 0) and its duration in *us, or 0 if none was made within
 * timeout_ms. One caller at a time (the recorder on the Camera screen).
 */
uint32_t gfx_copy_front(uint8_t *dst, uint32_t timeout_ms, uint32_t *us);

#endif
