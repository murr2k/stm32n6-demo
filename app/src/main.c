/*
 * STM32N6570-DK demo: entry point and UI loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dsp/sig.h"
#include "platform/ctl.h"
#include "platform/gfx.h"
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
	&screen_specimen,
};

int main(void)
{
	int ret = gfx_init();

	if (ret != 0) {
		LOG_ERR("display init failed (%d)", ret);
		return 0;
	}
	touch_init();
	sig_init();
	net_start();
	shell_init(screens, ARRAY_SIZE(screens));
	LOG_INF("ui up");

	for (;;) {
		int arg;

		switch (ctl_poll(&arg)) {
		case CTL_SCREEN:
			shell_goto(arg);
			break;
		case CTL_HUD:
			shell_toggle_hud();
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
