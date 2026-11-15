/*
 * Remote control service. See ctl.h for the protocol.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include <soc.h>
#include <stm32n6xx_hal.h>

#include "audio.h"
#include "ctl.h"
#include "dsp/dataset.h"
#include "dsp/sig.h"
#include "media/player.h"
#include "media/sdcard.h"
#include "gfx.h"
#include "mic.h"
#include "net.h"
#include "psram.h"
#include "screens/screens.h"
#include "touch.h"
#include "ui/shell.h"

LOG_MODULE_REGISTER(ctl, LOG_LEVEL_INF);

#define CTL_PORT 5556

struct ui_cmd {
	enum ctl_cmd cmd;
	int arg;
};

K_MSGQ_DEFINE(ui_q, sizeof(struct ui_cmd), 8, 4);

enum ctl_cmd ctl_poll(int *arg)
{
	struct ui_cmd c;

	if (k_msgq_get(&ui_q, &c, K_NO_WAIT) != 0) {
		return CTL_NONE;
	}
	*arg = c.arg;
	return c.cmd;
}

static void post(enum ctl_cmd cmd, int arg)
{
	struct ui_cmd c = {cmd, arg};

	k_msgq_put(&ui_q, &c, K_NO_WAIT);
}

static void swipe(int x1, int y1, int x2, int y2, int ms)
{
	int steps = MAX(ms / 8, 2);

	touch_inject(0, x1, y1, true);
	for (int i = 1; i <= steps; i++) {
		k_msleep(8);
		touch_inject(0, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, true);
	}
	k_msleep(8);
	touch_inject(0, x2, y2, false);
}

static int handle(char *line, char *out, size_t out_len)
{
	int a[5] = {0};
	char cmd[12] = {0};
	int n = sscanf(line, "%11s %d %d %d %d %d", cmd, &a[0], &a[1], &a[2], &a[3], &a[4]);

	if (n < 1) {
		return snprintf(out, out_len, "err empty\n");
	}
	if (!strcmp(cmd, "screen") && n >= 2) {
		post(CTL_SCREEN, a[0]);
	} else if (!strcmp(cmd, "hud")) {
		post(CTL_HUD, 0);
	} else if (!strcmp(cmd, "attract")) {
		post(CTL_ATTRACT, 0);
	} else if (!strcmp(cmd, "reset")) {
		gfx_stats_reset_worst();
	} else if (!strcmp(cmd, "tap") && n >= 3) {
		touch_inject(0, a[0], a[1], true);
		k_msleep(60);
		touch_inject(0, a[0], a[1], false);
	} else if (!strcmp(cmd, "swipe") && n >= 6) {
		swipe(a[0], a[1], a[2], a[3], a[4]);
	} else if (!strcmp(cmd, "touch") && n >= 5) {
		touch_inject(a[0], a[1], a[2], a[3] != 0);
	} else if (!strcmp(cmd, "peek") && n >= 2) {
		/* read one 32-bit word: bring-up aid, e.g. "peek 0x56028000" */
		uint32_t addr = strtoul(strchr(line, ' ') + 1, NULL, 0) & ~3u;

		return snprintf(out, out_len, "%08x: %08x\n", (unsigned int)addr, (unsigned int)*(volatile uint32_t *)addr);
	} else if (!strcmp(cmd, "dump") && n >= 2) {
		/* up to 64 words in hex, no separators: "dump 0x90200000 64" */
		char *p = strchr(line, ' ') + 1;
		const volatile uint32_t *w = (const volatile uint32_t *)(strtoul(p, &p, 0) & ~3u);
		uint32_t words = CLAMP(strtoul(p, NULL, 0), 1, 64);
		int len = 0;

		for (uint32_t i = 0; i < words; i++) {
			len += snprintf(out + len, out_len - len, "%08x", (unsigned int)w[i]);
		}
		return len + snprintf(out + len, out_len - len, "\n");
	} else if (!strcmp(cmd, "mic")) {
		struct mic_status m;

		mic_get_status(&m);
		return snprintf(out, out_len,
				"ok=%d total=%u blocks=%u errors=%u last_error=0x%x rate_hz=%u peak=%d rms=%d dc=%d\n", m.ok,
				m.total, m.blocks, m.errors, m.last_error, m.rate_hz, (int)m.peak, (int)m.rms, (int)m.dc);
	} else if (!strcmp(cmd, "sigsrc") && n >= 2) {
		sig_set_source(a[0] ? SIG_SRC_MIC : SIG_SRC_GENERATOR);
		return snprintf(out, out_len, "source=%d\n", sig_source());
	} else if (!strcmp(cmd, "micbits") && n >= 4) {
		return mic_bitbang(a[0], a[1], a[2], a[3], out, out_len);
	} else if (!strcmp(cmd, "micprobe")) {
		return mic_probe(out, out_len);
	} else if (!strcmp(cmd, "mictune")) {
		int v[10], k = sscanf(line, "%*s %d %d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
				      &v[6], &v[7], &v[8], &v[9]);

		return mic_tune(v, MAX(k, 0), out, out_len);
	} else if (!strcmp(cmd, "mcap")) {
		return mic_capture(n >= 2 ? a[0] : 0, out, out_len);
	} else if (!strcmp(cmd, "acap")) {
		return audio_capture(n >= 2 ? a[0] : 0, out, out_len);
	} else if (!strcmp(cmd, "saiclk")) {
		return snprintf(out, out_len,
				"CCIPR7=%08x IC7CFGR=%08x DIVENR=%08x PLL2CFGR1=%08x PLL2CFGR2=%08x PLL2CFGR3=%08x "
				"SAI_CR1=%08x SAI_FRCR=%08x hal_freq=%u\n",
				(unsigned int)RCC->CCIPR7, (unsigned int)RCC->IC7CFGR, (unsigned int)RCC->DIVENR,
				(unsigned int)RCC->PLL2CFGR1, (unsigned int)RCC->PLL2CFGR2, (unsigned int)RCC->PLL2CFGR3,
				(unsigned int)SAI1_Block_A->CR1, (unsigned int)SAI1_Block_A->FRCR,
				(unsigned int)HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SAI1));
	} else if (!strcmp(cmd, "tone") && n >= 3) {
		audio_tone(a[0], a[1]);
	} else if (!strcmp(cmd, "volume") && n >= 2) {
		audio_set_volume(a[0]);
	} else if (!strcmp(cmd, "bus") && n >= 3) {
		/* synthetic bus load for a[1] ms: 0 PSRAM read, 1 PSRAM write, 2 internal SRAM read,
		 * 3 flash read. Each pass covers 1 MB, far more than the data cache holds.
		 */
		static const uintptr_t base[] = {0x91000000, 0x91000000, 0x34249000, 0x70420000};
		volatile uint32_t *p = (volatile uint32_t *)base[a[0] & 3];
		uint32_t end = k_uptime_get_32() + a[1], sum = 0, passes = 0;

		while ((int32_t)(end - k_uptime_get_32()) > 0) {
			for (uint32_t i = 0; i < (1u << 18); i += 8) { /* one word per 32-byte cache line */
				if ((a[0] & 3) == 1) {
					p[i] = i;
				} else {
					sum += p[i];
				}
			}
			passes++;
		}
		return snprintf(out, out_len, "passes=%u sum=%08x\n", (unsigned int)passes, (unsigned int)sum);
	} else if (!strcmp(cmd, "arestart")) {
		audio_restart();
	} else if (!strcmp(cmd, "spin") && n >= 2) {
		/* keep the CPU out of idle for a while: tests whether sleep disturbs a peripheral */
		uint32_t end = k_uptime_get_32() + a[0];

		while ((int32_t)(end - k_uptime_get_32()) > 0) {
			k_busy_wait(100);
		}
	} else if (!strcmp(cmd, "arate")) {
		return audio_rate_probe(out, out_len);
	} else if (!strcmp(cmd, "audiodbg")) {
		return audio_debug(out, out_len);
	} else if (!strcmp(cmd, "audio")) {
		uint32_t blocks, errors;

		audio_get_counters(&blocks, &errors);
		return snprintf(out, out_len, "ok=%d blocks=%u errors=%u uptime_ms=%u\n", audio_ok(), blocks, errors,
				k_uptime_get_32());
	} else if (!strcmp(cmd, "video")) {
		struct player_status st;

		player_get_status(&st);
		return snprintf(out, out_len,
				"open=%d playing=%d frame=%u n_frames=%u dropped=%u decode_us=%u convert_us=%u "
				"kbps=%u audio_clock=%d card=%d buffering=%d stalls=%u ahead_kb=%u seek_us=%u\n",
				st.open, st.playing, st.frame, st.n_frames, st.dropped, st.decode_us, st.convert_us,
				st.kbps, st.audio_clock, st.on_card, st.buffering, st.stalls, st.ahead_kb, st.seek_us);
	} else if (!strcmp(cmd, "media")) {
		/* the clip list: how many, and the first one that is read from the card */
		struct sd_status sd;
		int clips = player_clip_count(), first_card = -1, on_card = 0;

		sdcard_get_status(&sd);
		for (int i = clips - 1; i >= 0; i--) {
			if (player_clip_on_card(i)) {
				first_card = i;
				on_card++;
			}
		}
		return snprintf(out, out_len,
				"mounted=%d clips=%d card_clips=%d first_card=%d total_mb=%u free_mb=%u gen=%u "
				"receiving=%d psram_kb=%u\n",
				sd.mounted, clips, on_card, first_card, sd.total_mb, sd.free_mb, sd.generation,
				sd.receiving, (unsigned int)(psram_used() / 1024));
	} else if (!strcmp(cmd, "card") && n >= 2) {
		player_use_card(a[0] != 0);
	} else if (!strcmp(cmd, "clip") && n >= 2) {
		const struct n6v_header *h = player_clip(a[0]);

		if (h == NULL) {
			return snprintf(out, out_len, "err no such clip\n");
		}
		/* file first: it has no spaces, the title may */
		return snprintf(out, out_len, "file=%s card=%d frames=%u title=%s\n",
				player_clip_on_card(a[0]) ? player_clip_file(a[0]) : "-", player_clip_on_card(a[0]),
				h->n_frames, h->title);
	} else if (!strcmp(cmd, "play") && n >= 2) {
		post(CTL_PLAY, a[0]);
	} else if (!strcmp(cmd, "ds")) {
		return ds_info(out, out_len);
	} else if (!strcmp(cmd, "hometiles") && n >= 2) {
		post(CTL_HOME_TILES, a[0]);
	} else if (!strcmp(cmd, "home")) {
		int tiles, scroll, max;

		home_get_status(&tiles, &scroll, &max);
		return snprintf(out, out_len, "tiles=%d scroll=%d max=%d\n", tiles, scroll, max);
	} else if (!strcmp(cmd, "where")) {
		/* which screen is up, and whether the demo is running itself */
		return snprintf(out, out_len, "screen=%d attract=%d\n", shell_current(), shell_in_attract());
	} else if (!strcmp(cmd, "stats")) {
		const struct gfx_stats *g = gfx_stats();

		return snprintf(out, out_len,
				"frames=%u fps=%u.%u render_avg_us=%u render_last_us=%u "
				"render_worst_us=%u period_worst_us=%u dropped=%u refresh_mhz=%u\n",
				g->frames, g->fps_x10 / 10, g->fps_x10 % 10, g->avg_render_us,
				g->last_render_us, g->worst_render_us, g->worst_period_us, g->dropped,
				g->refresh_uhz / 1000);
	} else {
		return snprintf(out, out_len, "err unknown: %s\n", cmd);
	}
	return snprintf(out, out_len, "ok\n");
}

static void ctl_thread(void *p1, void *p2, void *p3)
{
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(CTL_PORT)};
	static char line[96], out[640];
	int srv;

	net_wait_ip();
	srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (srv < 0 || zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    zsock_listen(srv, 1) < 0) {
		LOG_ERR("socket setup failed (%d)", errno);
		return;
	}
	LOG_INF("control on tcp/%d", CTL_PORT);
	for (;;) {
		int s = zsock_accept(srv, NULL, NULL);
		size_t len = 0;

		if (s < 0) {
			continue;
		}
		/* One client at a time, so a client that vanishes without a clean close
		 * (one lost FIN is enough) must not hold the port for ever: without
		 * these the thread sat in recv() and the board looked dead from outside.
		 */
		struct zsock_timeval idle = {.tv_sec = 5};

		zsock_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof(idle));
		zsock_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &idle, sizeof(idle));
		for (;;) {
			char ch;

			if (zsock_recv(s, &ch, 1, 0) <= 0) {
				break;
			}
			if (ch == '\n' || len == sizeof(line) - 1) {
				line[len] = '\0';
				len = 0;
				int n = handle(line, out, sizeof(out));

				if (zsock_send(s, out, n, 0) < 0) {
					break;
				}
			} else if (ch != '\r') {
				line[len++] = ch;
			}
		}
		zsock_close(s);
	}
}
K_THREAD_DEFINE(ctl, 4096, ctl_thread, NULL, NULL, NULL, 9, 0, 0);
