/*
 * Display platform.
 *
 * LVGL renders in DIRECT mode into two full-size ARGB8888 frame buffers in
 * internal SRAM. On the last flush of a frame the finished buffer is handed to
 * the LTDC driver, which swaps the layer pointer in the line interrupt and
 * blocks this thread until the swap happened. So: no copy, no tearing, and the
 * UI thread is phase-locked to the panel while anything is animating.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <soc.h>
#include <stm32n6xx_hal.h>

#include "gfx.h"
#include "media/jpegdec.h" /* dma2d_copy32 */
#include "prof.h"
#include "psram.h"

LOG_MODULE_REGISTER(gfx, LOG_LEVEL_INF);

/* See the memory map in boards/stm32n6570_dk.overlay. */
#define FB_A ((uint8_t *)0x340D2000)
#define FB_B ((uint8_t *)GFX_FB_B)
/* Stable copy for screenshots, in PSRAM (taken from psram_alloc in gfx_init). */
static uint8_t *snapshot;

BUILD_ASSERT((uintptr_t)FB_B - (uintptr_t)FB_A == GFX_FB_BYTES);
BUILD_ASSERT((uintptr_t)FB_B + GFX_FB_BYTES == 0x343C0000);

static const struct device *const panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static lv_display_t *disp;
static struct gfx_stats stats;

static const uint8_t *front;      /* buffer the LTDC is scanning out */
static uint32_t t_frame_start;     /* end of the previous frame slot */
static uint32_t t_last_present;
static bool presented;            /* a frame was presented in this gfx_run_frame() */
static bool streak;               /* previous frame slot also presented */

static uint32_t win_frames, win_render_us, win_start_us;

/* the traces (gfx.h), and what each frame's entry is made from */
static struct gfx_trace trace = {.n = GFX_TRACE_N};
static struct gfx_drop last_drop;
static struct gfx_trace_frame recent[GFX_DROP_N]; /* ring, the frame just handed over at recent_at */
static uint8_t recent_at;
static int8_t on_screen = -1;
static uint8_t fade_alpha = 255;
static uint32_t wvpcr0; /* the layer window the driver set up (gfx_set_offset) */
static uint32_t t_run_start;    /* gfx_run_frame() entered */
static uint32_t last_wake_us;   /* the last swap to the UI thread running */
static uint32_t t_deadline;     /* the next frame must be handed over by then */
static uint32_t last_tail_us;   /* that present's tail, 0 if the frame was not presented */
static uint32_t ui_cyc_present; /* UI thread cycles at that present */

/* lending frame buffer B: see gfx.h */
enum lend_state { DOUBLE, ENTERING, SINGLE, RETURNING };
static enum lend_state lend_state = DOUBLE; /* UI thread only */
static bool lend_want;                      /* UI thread only */
static atomic_t lent;                       /* FB B may be used by the borrower */
static atomic_t in_use;                     /* the borrower holds it right now */

static atomic_ptr_t reader; /* a buffer a bus master is reading (gfx_reader_begin) */
static K_SEM_DEFINE(reader_done, 0, 1);

static atomic_t snap_request;
static K_SEM_DEFINE(snap_done, 0, 1);
static K_MUTEX_DEFINE(snap_lock);

/* gfx_copy_front(): the destination asked for, and the copy's time */
static atomic_ptr_t copy_dst;
static K_SEM_DEFINE(copy_done, 0, 1);
static uint32_t copy_t0, copy_us; /* written before copy_done is given */
static bool copy_ok;

uint32_t gfx_now_us(void)
{
	return (uint32_t)k_cyc_to_us_floor64(k_cycle_get_64());
}

static uint32_t tick_cb(void)
{
	return k_uptime_get_32();
}


static void account_frame(uint32_t render_us, uint32_t now)
{
	uint32_t period = now - t_last_present;

	t_last_present = now;
	stats.frames++;
	stats.last_render_us = render_us;
	stats.render_us[stats.head] = MIN(render_us, UINT16_MAX);

	if (streak) {
		/* Consecutive frames: the interval is a real frame time. */
		stats.last_period_us = period;
		stats.period_us_ring[stats.head] = MIN(period, UINT16_MAX);
		if (period > stats.worst_period_us) {
			stats.worst_period_us = period;
		}
		if (period > stats.period_us + stats.period_us / 2) {
			stats.dropped += (period + stats.period_us / 2) / stats.period_us - 1;
			/* zprof's flight recorder, when armed, keeps what led up to it */
			ZPROF_TRIGGER(1);
			last_drop.frame = stats.frames;
			last_drop.seq = trace.seq;
			last_drop.screen = on_screen;
			for (int i = 0; i < GFX_DROP_N; i++) {
				last_drop.f[i] = recent[(recent_at + 1 + i) % GFX_DROP_N];
			}
		}
	} else {
		stats.period_us_ring[stats.head] = 0;
	}
	if (render_us > stats.worst_render_us) {
		stats.worst_render_us = render_us;
	}
	stats.head = (stats.head + 1) % GFX_HISTORY;

	win_frames++;
	win_render_us += render_us;
	if (now - win_start_us >= 1000000) {
		stats.fps_x10 = (uint32_t)((uint64_t)win_frames * 10000000 / (now - win_start_us));
		stats.avg_render_us = win_render_us / win_frames;
		win_frames = 0;
		win_render_us = 0;
		win_start_us = now;
	}
}

/* Single-buffered pacing: sleep until shortly before the LTDC's line counter
 * wraps (start of vertical sync, the same point the double-buffered swap
 * waits for), then watch it wrap. The position before the sleep is the
 * reference: if a cooperative thread kept the UI asleep past the wrap, it
 * returns at once instead of waiting a whole refresh (a dropped frame). */
static void wait_refresh_start(void)
{
	uint32_t total = (LTDC->TWCR & LTDC_TWCR_TOTALH) + 1;
	uint32_t y = LTDC->CPSR & LTDC_CPSR_CYPOS;
	uint32_t line_us = MAX(stats.period_us / total, 1u);
	uint32_t left_us = (total - MIN(y, total)) * line_us;

	if (left_us > 1500) {
		k_usleep(left_us - 1000);
	}
	uint32_t prev = y;

	for (uint32_t t0 = gfx_now_us(); gfx_now_us() - t0 < 20000;) {
		uint32_t cur = LTDC->CPSR & LTDC_CPSR_CYPOS;

		if (cur < prev) {
			return;
		}
		prev = cur;
	}
}

static uint32_t ui_cycles(void)
{
	k_thread_runtime_stats_t st;

	return k_thread_runtime_stats_get(k_current_get(), &st) == 0 ? (uint32_t)st.execution_cycles : 0;
}

/* At the hand-over of a frame: how much of its budget was left, and where
 * the rest went. */
static void trace_frame(uint32_t now)
{
	int32_t left = (int32_t)(t_deadline - now);
	uint32_t ran = (uint32_t)k_cyc_to_us_floor64(ui_cycles() - ui_cyc_present);
	uint32_t wall = now - t_last_present;

	if (streak && left < stats.margin_min_us) {
		stats.margin_min_us = left;
	}
	if (streak) {
		ZPROF_PLOT("frame.margin_us", left);   /* what margin_min_us is the least of */
	}
	recent_at = (recent_at + 1) % GFX_DROP_N;

	struct gfx_trace_frame *f = &recent[recent_at];

	f->wake_us = MIN(last_wake_us, UINT16_MAX);
	f->tail_us = MIN(last_tail_us, UINT16_MAX);
	f->pre_us = MIN(t_run_start - t_frame_start, UINT16_MAX);
	f->lv_us = MIN(now - t_run_start, UINT16_MAX);
	f->away_us = MIN(wall > ran ? wall - ran : 0, UINT16_MAX);
	f->left_us = (int16_t)CLAMP(left, INT16_MIN, INT16_MAX);
	f->alpha = fade_alpha;
	f->locked = streak;
	if (trace.n < GFX_TRACE_N) {
		trace.f[trace.n++] = *f;
	}
}

/*
 * Presenting a frame. The panel's refresh, in LTDC lines: line 0 starts the
 * vertical sync, the active area runs from act0 to act1 (12 to 491 of 501
 * here), and the lines from act1 + 1 round to act0 - 1 are blanking, where
 * the layer may change buffers without tearing.
 *
 * This is done here rather than by display_write(): the driver cleans the
 * data cache by address over the whole 1.5 MB buffer (about 540 us a frame,
 * nearly all of it the loop, the lines being clean already) and swaps at
 * line 0, the middle of the blanking. A clean of the whole cache by set and
 * way takes 25 us, and a frame finished in the blanking can still be shown
 * in the next refresh: about 1 ms more for every frame.
 */
#define SWAP_GUARD 2 /* lines before the active area that an immediate swap must keep */

static uint32_t ln_total, ln_act0, ln_act1, line_ns;

static uint32_t lines_us(uint32_t lines)
{
	return (uint32_t)((uint64_t)lines * line_ns / 1000);
}

static uint32_t panel_line(void)
{
	return LTDC->CPSR & LTDC_CPSR_CYPOS;
}

/* Swap to buf (its lines already cleaned from the cache) and return once the
 * panel will show it in its next active area: at once in the blanking,
 * otherwise at its start (the shadow registers' reload at vertical
 * blanking). */
static void present(const uint8_t *buf)
{
	if (buf == front) {
		return;
	}
	LTDC_Layer1->CFBAR = (uint32_t)buf;

	/* no interrupt between reading the line and the reload */
	unsigned int key = irq_lock();
	uint32_t y = panel_line();
	bool blanking = y > ln_act1 || y + SWAP_GUARD < ln_act0;

	if (blanking) {
		LTDC->SRCR = LTDC_SRCR_IMR;
	}
	irq_unlock(key);
	if (blanking) {
		if (streak && y < ln_act0) {
			stats.late_swaps++;
		}
	} else {
		LTDC->SRCR = LTDC_SRCR_VBR;
		uint32_t wait = lines_us(ln_act1 + 1 - y);

		if (wait > 300) {
			k_usleep(wait - 200);
		}
		for (uint32_t t0 = gfx_now_us(); (LTDC->SRCR & LTDC_SRCR_VBR) && gfx_now_us() - t0 < 40000;) {
		}
	}
	front = buf;
}

/* gfx_copy_front(), on the UI thread where the front buffer is whole and
 * nothing draws into it */
static void service_copy(void)
{
	if (atomic_ptr_get(&copy_dst) == NULL) {
		return;
	}
	/* taken, so a requester that gives up now sees it is being made */
	uint8_t *dst = atomic_ptr_set(&copy_dst, NULL);

	if (dst == NULL) {
		return;
	}
	copy_t0 = gfx_now_us();
	copy_ok = dma2d_copy32(front, dst, GFX_W, GFX_H, &copy_us) == 0;
	k_sem_give(&copy_done);
}

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map)
{
	ARG_UNUSED(area);

	if (lv_display_flush_is_last(d)) {
		SCB_CleanDCache(); /* what LVGL wrote, out to the SRAM the LTDC reads */

		uint32_t now = gfx_now_us();
		/* everything the UI thread did for this frame: screen tick, layout, render */
		uint32_t render_us = now - t_frame_start;
		bool swap = px_map != front;

		trace_frame(now);
		/* zprof's frame job: from the last hand-over (or idle slot) to this one */
		ZPROF_MARK_STOP("frame", 1);
		present(px_map);
		ZPROF_MARK_START("frame");
		if (lend_state == ENTERING && front == FB_A) {
			/* the panel scans A, LVGL draws into A: B is free */
			lend_state = SINGLE;
			atomic_set(&lent, 1);
		} else if (lend_state == SINGLE || lend_state == RETURNING) {
			/* the buffer did not change, so nothing waited for the refresh:
			 * start the next frame at the top of the next one. A copy asked
			 * for is made first, in that wait; one that ran past the top
			 * starts the next frame at once instead of a refresh later. */
			uint32_t y0 = panel_line();

			service_copy();
			if (panel_line() >= y0) {
				wait_refresh_start();
			}
		}
		presented = true;
		now = gfx_now_us();

		uint32_t y = panel_line();

		if (swap) {
			/* shown from the next start of the active area; the frame after
			 * it has until shortly before the start of the one after that */
			int32_t to_show = y > ln_act1 ? (int32_t)(ln_total - y + ln_act0) : (int32_t)ln_act0 - (int32_t)y;

			last_wake_us = y > ln_act1 ? lines_us(y - ln_act1 - 1) : 0;
			t_deadline = now + (uint32_t)((int32_t)lines_us(ln_total - SWAP_GUARD) +
						      (to_show >= 0 ? (int32_t)lines_us(to_show)
								    : -(int32_t)lines_us(-to_show)));
		} else {
			/* single-buffered: the next frame must start before the next refresh does */
			last_wake_us = lines_us(MIN(y, ln_total));
			t_deadline = now - last_wake_us + stats.period_us;
		}
		ui_cyc_present = ui_cycles();
		account_frame(render_us, now);
	}
	lv_display_flush_ready(d);
}

static void lv_log_cb(lv_log_level_t level, const char *buf)
{
	if (level >= LV_LOG_LEVEL_ERROR) {
		LOG_ERR("%s", buf);
	} else {
		LOG_WRN("%s", buf);
	}
}

/* Measure the real refresh period by swapping two blank buffers. */
static void measure_refresh(void)
{
	const struct display_buffer_descriptor desc = {
		.buf_size = GFX_FB_BYTES, .width = GFX_W, .height = GFX_H, .pitch = GFX_W,
	};
	const int n = 30;
	uint32_t t0;

	display_write(panel, 0, 0, &desc, FB_A);
	display_write(panel, 0, 0, &desc, FB_B);
	t0 = gfx_now_us();
	for (int i = 0; i < n; i++) {
		display_write(panel, 0, 0, &desc, (i & 1) ? FB_B : FB_A);
	}
	stats.period_us = (gfx_now_us() - t0) / n;
	stats.refresh_uhz = (uint32_t)(1000000000000ULL / stats.period_us);
	/* from here on present() swaps, not the driver */
	front = (const uint8_t *)LTDC_Layer1->CFBAR;
	ln_total = (LTDC->TWCR & LTDC_TWCR_TOTALH) + 1;
	ln_act0 = (LTDC->BPCR & LTDC_BPCR_AVBP) + 1;
	ln_act1 = LTDC->AWCR & LTDC_AWCR_AAH;
	line_ns = stats.period_us * 1000 / ln_total;
	t_deadline = gfx_now_us() + stats.period_us;
}

int gfx_init(void)
{
	if (!device_is_ready(panel)) {
		LOG_ERR("display not ready");
		return -ENODEV;
	}

	snapshot = psram_alloc(GFX_FB_BYTES, 64);
	if (snapshot == NULL) {
		return -ENOMEM;
	}
	memset(FB_A, 0, GFX_FB_BYTES);
	memset(FB_B, 0, GFX_FB_BYTES);
	measure_refresh();
	LOG_INF("panel refresh %u.%03u Hz (period %u us)", stats.refresh_uhz / 1000000,
		(stats.refresh_uhz / 1000) % 1000, stats.period_us);

	lv_init();
	lv_tick_set_cb(tick_cb);
	lv_log_register_print_cb(lv_log_cb);

	disp = lv_display_create(GFX_W, GFX_H);
	if (disp == NULL) {
		return -ENOMEM;
	}
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, FB_A, FB_B, GFX_FB_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush_cb);

	t_last_present = win_start_us = t_frame_start = gfx_now_us();
	stats.margin_min_us = INT32_MAX;
	wvpcr0 = LTDC_Layer1->WVPCR;
	/* what shows through when the layer is faded: the theme's screen colour (0x080C12) */
	LTDC->BCCR = 0x080C12;
	LTDC->SRCR = LTDC_SRCR_VBR;
	display_blanking_off(panel);
	return 0;
}

static void service_snapshot(void)
{
	if (atomic_cas(&snap_request, 1, 0)) {
		/* Between frames the front buffer is not being drawn into. */
		memcpy(snapshot, front, GFX_FB_BYTES);
		k_sem_give(&snap_done);
	}
}

/* Called at the start of every frame, before LVGL runs: moves the lending
 * state machine on. */
static void lend_step(void)
{
	lv_obj_t *scr = lv_screen_active();

	switch (lend_state) {
	case DOUBLE:
		if (lend_want) {
			/* draw everything into A; the flush shows A, then B is free */
			lv_display_set_buffers(disp, FB_A, NULL, GFX_FB_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
			lv_obj_invalidate(scr);
			lend_state = ENTERING;
		}
		break;
	case ENTERING:
	case SINGLE:
		if (!lend_want) {
			atomic_set(&lent, 0); /* no new borrowing */
			lend_state = RETURNING;
		}
		break;
	case RETURNING:
		if (lend_want) {
			atomic_set(&lent, 1);
			lend_state = SINGLE;
		} else if (atomic_get(&in_use) == 0) {
			/* B holds whatever the NPU left: draw all of it, show it, and LVGL
			 * then copies it to A as it does for every frame */
			lv_display_set_buffers(disp, FB_B, FB_A, GFX_FB_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
			lv_obj_invalidate(scr);
			lend_state = DOUBLE;
		}
		break;
	}
}

void gfx_lend_back_buffer(bool lend)
{
	lend_want = lend;
}

bool gfx_borrow_begin(void)
{
	atomic_inc(&in_use);
	if (!atomic_get(&lent)) {
		atomic_dec(&in_use);
		return false;
	}
	return true;
}

void gfx_borrow_end(void)
{
	atomic_dec(&in_use);
}

bool gfx_back_buffer_lent(void)
{
	return atomic_get(&lent) != 0;
}

/* The buffer this frame draws into: the one not on screen, or A when
 * single-buffered. */
static const uint8_t *draw_target(void)
{
	return lend_state != DOUBLE ? FB_A : front == FB_A ? FB_B : FB_A;
}

/* Do not draw into a buffer a reader still has (gfx_reader_begin). */
static void wait_reader(void)
{
	const uint8_t *target = draw_target();

	if (atomic_ptr_get(&reader) != target) {
		return;
	}
	uint32_t t0 = gfx_now_us();

	while (atomic_ptr_get(&reader) == target && gfx_now_us() - t0 < 30000) {
		k_sem_take(&reader_done, K_MSEC(5));
	}
	uint32_t us = gfx_now_us() - t0;

	stats.reader_waits++;
	stats.reader_wait_worst_us = MAX(stats.reader_wait_worst_us, us);
}

void gfx_run_frame(void)
{
	uint32_t t0 = gfx_now_us();

	t_run_start = t0;
	lend_step();
	wait_reader();

	presented = false;
	lv_timer_handler();
	service_snapshot();
	if (!presented || lend_state == DOUBLE) {
		/* single-buffered with a frame presented, flush_cb() did it before
		 * waiting for the refresh; here it would delay the next frame */
		service_copy();
	}

	if (presented) {
		uint32_t tail = gfx_now_us() - t_last_present;

		streak = true;
		last_tail_us = tail;
		if (tail > stats.worst_tail_us) {
			stats.worst_tail_us = tail;
		}
	} else {
		streak = false;
		last_tail_us = 0;
		/* Nothing to draw: idle part of a frame slot so timers and input keep
		 * their cadence and lower-priority threads get to run.
		 */
		uint32_t spent = gfx_now_us() - t0;

		ZPROF_MARK_STOP("frame", 0); /* a slot with nothing to hand over: no frame */
		k_sleep(K_USEC(spent < stats.period_us ? MIN(stats.period_us - spent, 4000) : 500));
		ZPROF_MARK_START("frame");
	}
	t_frame_start = gfx_now_us();
}

const uint8_t *gfx_snapshot(void)
{
	k_mutex_lock(&snap_lock, K_FOREVER);
	k_sem_reset(&snap_done);
	atomic_set(&snap_request, 1);
	k_sem_take(&snap_done, K_MSEC(500));
	k_mutex_unlock(&snap_lock);
	return snapshot;
}

const struct gfx_stats *gfx_stats(void)
{
	return &stats;
}

void gfx_set_fade(uint8_t alpha)
{
	fade_alpha = alpha;
	LTDC_Layer1->CACR = alpha;
	LTDC->SRCR = LTDC_SRCR_VBR; /* load the shadow register at the next vertical blank */
}

void gfx_set_offset(int rows)
{
	rows = CLAMP(rows, 0, GFX_H / 2);
	LTDC_Layer1->WVPCR = (wvpcr0 & ~LTDC_LxWVPCR_WVSTPOS_Msk) |
			     ((wvpcr0 & LTDC_LxWVPCR_WVSTPOS_Msk) + (uint32_t)rows);
	LTDC->SRCR = LTDC_SRCR_VBR;
}

uint32_t gfx_last_present_us(void)
{
	return t_last_present;
}

void gfx_stats_reset_worst(void)
{
	stats.worst_render_us = 0;
	stats.worst_period_us = 0;
	stats.worst_tail_us = 0;
	stats.margin_min_us = INT32_MAX;
	stats.reader_wait_worst_us = 0;
}

void gfx_trace_begin(int from, int to)
{
	trace.seq++;
	trace.from = (int8_t)from;
	trace.to = (int8_t)to;
	trace.switch_us = 0;
	trace.n = 0;
	trace.at_switch = GFX_TRACE_N;
}

void gfx_trace_switch(int to, uint32_t switch_us)
{
	on_screen = (int8_t)to;
	if (trace.n < GFX_TRACE_N && trace.to == to) {
		trace.at_switch = trace.n;
		trace.switch_us = (uint16_t)MIN(switch_us, UINT16_MAX);
	}
}

const struct gfx_trace *gfx_trace(void)
{
	return &trace;
}

const struct gfx_drop *gfx_last_drop(void)
{
	return &last_drop;
}

uint32_t gfx_sleep_to_active(void)
{
	uint32_t y = panel_line();
	/* to a line into the next active area: whatever was swapped in the
	 * blanking before it is on screen then, and stays until the next one */
	uint32_t lines = y < ln_act0 ? ln_act0 - y : ln_total - y + ln_act0;

	k_usleep((int32_t)lines_us(lines + 1));
	return gfx_now_us();
}

const uint8_t *gfx_front(void)
{
	return front;
}

bool gfx_single_buffered(void)
{
	return lend_state != DOUBLE;
}

void gfx_reader_begin(const uint8_t *buf)
{
	k_sem_reset(&reader_done);
	atomic_ptr_set(&reader, (atomic_ptr_val_t)buf);
}

void gfx_reader_end(void)
{
	atomic_ptr_set(&reader, NULL);
	k_sem_give(&reader_done);
}

uint32_t gfx_copy_front(uint8_t *dst, uint32_t timeout_ms, uint32_t *us)
{
	k_sem_reset(&copy_done);
	atomic_ptr_set(&copy_dst, dst);
	if (k_sem_take(&copy_done, K_MSEC(timeout_ms)) != 0) {
		/* taken back unless the UI thread took it already: then it is being made */
		if (atomic_ptr_cas(&copy_dst, dst, NULL)) {
			return 0;
		}
		k_sem_take(&copy_done, K_MSEC(100));
	}
	if (us) {
		*us = copy_us;
	}
	return copy_ok ? MAX(copy_t0, 1u) : 0;
}

lv_display_t *gfx_display(void)
{
	return disp;
}
