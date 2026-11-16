/*
 * STM32N6570-DK demo: entry point and UI loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dsp/sig.h"
#include "platform/audio.h"
#include "platform/ctl.h"
#include "platform/gfx.h"
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
	&screen_hood,
	&screen_settings,
	&screen_camera,
};

int main(void)
{
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
	shell_init(screens, ARRAY_SIZE(screens));
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
		default:
			break;
		}
		tele_poll(k_uptime_get_32());
		shell_tick();
		gfx_run_frame();
	}
	return 0;
}
