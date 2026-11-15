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

LOG_MODULE_REGISTER(gfx, LOG_LEVEL_INF);

/* See the memory map in boards/stm32n6570_dk.overlay. */
#define FB_A ((uint8_t *)0x340D2000)
#define FB_B ((uint8_t *)0x34249000)
/* Stable copy for screenshots: first 1.5 MB of the PSRAM. */
#define FB_SNAPSHOT ((uint8_t *)0x90000000)

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

static atomic_t snap_request;
static K_SEM_DEFINE(snap_done, 0, 1);
static K_MUTEX_DEFINE(snap_lock);

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

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map)
{
	ARG_UNUSED(area);

	if (lv_display_flush_is_last(d)) {
		const struct display_buffer_descriptor desc = {
			.buf_size = GFX_FB_BYTES, .width = GFX_W, .height = GFX_H, .pitch = GFX_W,
		};
		/* everything the UI thread did for this frame: screen tick, layout, render */
		uint32_t render_us = gfx_now_us() - t_frame_start;

		/* Blocks until the LTDC has switched to this buffer. */
		display_write(panel, 0, 0, &desc, px_map);
		front = px_map;
		presented = true;
		account_frame(render_us, gfx_now_us());
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
	front = FB_A;
}

int gfx_init(void)
{
	if (!device_is_ready(panel)) {
		LOG_ERR("display not ready");
		return -ENODEV;
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
		memcpy(FB_SNAPSHOT, front, GFX_FB_BYTES);
		k_sem_give(&snap_done);
	}
}

void gfx_run_frame(void)
{
	uint32_t t0 = gfx_now_us();

	presented = false;
	lv_timer_handler();
	service_snapshot();

	if (presented) {
		streak = true;
	} else {
		streak = false;
		/* Nothing to draw: idle part of a frame slot so timers and input keep
		 * their cadence and lower-priority threads get to run.
		 */
		uint32_t spent = gfx_now_us() - t0;

		k_sleep(K_USEC(spent < stats.period_us ? MIN(stats.period_us - spent, 4000) : 500));
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
	return FB_SNAPSHOT;
}

const struct gfx_stats *gfx_stats(void)
{
	return &stats;
}

void gfx_set_fade(uint8_t alpha)
{
	LTDC_Layer1->CACR = alpha;
	LTDC->SRCR = LTDC_SRCR_VBR; /* load the shadow register at the next vertical blank */
}

uint32_t gfx_last_present_us(void)
{
	return t_last_present;
}

void gfx_stats_reset_worst(void)
{
	stats.worst_render_us = 0;
	stats.worst_period_us = 0;
}

lv_display_t *gfx_display(void)
{
	return disp;
}
