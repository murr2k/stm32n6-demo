/*
 * STM32N6570-DK demo: entry point and UI loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <soc.h>
#include <stm32n6xx_hal.h>

#include "dsp/sig.h"
#include "platform/audio.h"
#include "platform/ctl.h"
#include "platform/gfx.h"
#include "platform/homepower.h"
#include "platform/mic.h"
#include "platform/net.h"
#include "platform/tele.h"
#include "platform/touch.h"
#include "screens/screens.h"
#include "ui/shell.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

static const struct screen *const screens[] = {
	&screen_home,
	&screen_deck,
	&screen_siglab,
	&screen_explorer,
	&screen_touchlab,
	&screen_hostlink,
	&screen_coremap,
	&screen_cinema,
	&screen_specimen,
	&screen_settings,
	&screen_camera,
	&screen_power, /* listed only while the energy monitor answers */
};

/* The attract lap: "N6: the film" up to its last scene, the demos, each
 * arriving on a bar line of the film's music (2.4 s a bar, 100 BPM), then the
 * film's last scene and the music's coda. One clip on the card carries the
 * music for the whole lap (media/film/attract.py makes it; its DEMOS lists
 * the same screens and bars): Cinema plays it, and it plays on, sound only,
 * under the demos. The ghost plays the theremin quietly over a break. After
 * Signal Lab, Cinema shows four bars of the clip's picture: the Camera screen
 * tracking two hands, recorded on this board (the lap does not open the
 * camera itself). After the coda, end titles (7.2 s, then 12 s held and a
 * 1.5 s fade) and 2 s of black: the lap's end is not on a bar line.
 * Without the clip (no card), or with one of another length, the screens
 * take turns as before. */
#define BAR_MS 2400
#define LAP_MS (52 * BAR_MS + 22700)

static const struct shell_step lap_steps[] = {
	{&screen_cinema, 0},              /* the film to 0:43.2 */
	{&screen_home, 18 * BAR_MS},
	{&screen_deck, 20 * BAR_MS},
	{&screen_siglab, 24 * BAR_MS},    /* three bars: the break, the ghost's flight */
	{&screen_cinema, 27 * BAR_MS},    /* the recorded gestures */
	{&screen_explorer, 31 * BAR_MS},
	{&screen_hostlink, 39 * BAR_MS},
	{&screen_coremap, 43 * BAR_MS},
	{&screen_cinema, 50 * BAR_MS},    /* the film's last scene, the coda, the end titles */
};

static bool lap_available(void)
{
	uint32_t ms = cinema_lap_length_ms();

	if (ms != 0 && ms != LAP_MS) {
		LOG_WRN("the attract lap's clip is %u ms, the lap %u ms: the screens take turns instead",
			ms, LAP_MS);
	}
	return ms == LAP_MS;
}

static const struct shell_program lap = {
	.steps = lap_steps,
	.n_steps = ARRAY_SIZE(lap_steps),
	.lap_ms = LAP_MS,
	.available = lap_available,
	.start = cinema_lap_start,
	.clock_ms = cinema_lap_ms,
};

/* Lock the secure and privileged attributes that Zephyr gave the bus slaves
 * at boot (soc_rif_config in its N6 SoC code) until the next reset. Why:
 * SEGGER SystemView's stop of a J-Link recording (V4.10b, J-Link V9.96a)
 * writes 0 into RIFSC RISC_SECCFGR3 from the debug port, measured
 * 2026-11-21 within 0.15 s of the stop. That makes LTDC layer 1 non-secure;
 * its reads of the frame buffers are then refused by the AXISRAM firewalls
 * (RISAF2/3 log them) and come back as zeros, alpha 0: the panel shows the
 * near-black background until a power cycle, while everything else runs on.
 * The same register holds JPEG, VENC, DMA2D and the NPU. Nothing in the app
 * changes these slaves afterwards (sdcard.c sets SDMMC2's own, not locked). */
static void rif_lock(void)
{
	static const uint32_t slaves[] = {
		RIF_RISC_PERIPH_INDEX_ADC12, RIF_RISC_PERIPH_INDEX_DCMIPP, RIF_RISC_PERIPH_INDEX_DMA2D,
		RIF_RISC_PERIPH_INDEX_ETH1, RIF_RISC_PERIPH_INDEX_JPEG, RIF_RISC_PERIPH_INDEX_LTDCL1,
#ifdef NPU_PRESENT
		RIF_RISC_PERIPH_INDEX_NPU,
#endif
		RIF_RISC_PERIPH_INDEX_VENC,
	};

	for (size_t i = 0; i < ARRAY_SIZE(slaves); i++) {
		HAL_RIF_RISC_SlaveConfigLock(slaves[i]);
	}
}

int main(void)
{
	rif_lock();

	int ret = gfx_init();

	if (ret != 0) {
		LOG_ERR("display init failed (%d)", ret);
		return 0;
	}
	touch_init();
	if (audio_init() != 0) {
		LOG_WRN("no audio output; video will run on the system clock");
	}
	if (mic_init() != 0) {
		LOG_WRN("no microphone; Signal Lab has its generator only");
	}
	sig_init();
	net_start();
	homepower_start();
	shell_init(screens, ARRAY_SIZE(screens));
	shell_set_program(&lap);
	LOG_INF("ui up");

	for (;;) {
		int arg;

		switch (ctl_poll(&arg)) {
		case CTL_SCREEN:
			shell_note_input();
			shell_goto(arg);
			break;
		case CTL_HUD:
			shell_toggle_hud();
			break;
		case CTL_ATTRACT:
			shell_start_attract();
			break;
		case CTL_PLAY:
			shell_note_input();
			for (int i = 0; i < (int)ARRAY_SIZE(screens); i++) {
				if (screens[i] == &screen_cinema) {
					shell_goto(i);
				}
			}
			cinema_play(arg);
			break;
		case CTL_HOME_TILES:
			home_test_tiles(arg);
			break;
		case CTL_CRASH:
			/* a fault on the UI thread, on purpose (ctl.c "crash 4"): essential,
			 * so zprof's handler halts the board; n6.cmd fault reads the record */
			__asm__ volatile("udf #1");
			break;
		case CTL_INPUT:
			shell_note_input();
			break;
		default:
			break;
		}
		uint32_t t0 = gfx_now_us();

		tele_poll(k_uptime_get_32());
		shell_cost_tele(gfx_now_us() - t0);
		shell_tick();
		gfx_run_frame();
	}
	return 0;
}
