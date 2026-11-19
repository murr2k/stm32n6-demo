/*
 * Remote control service. See ctl.h for the protocol.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
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
#include "dsp/monitor.h"
#include "dsp/sig.h"
#include "dsp/theremin.h"
#include "media/player.h"
#include "media/recorder.h"
#include "media/sdcard.h"
#include "gfx.h"
#include "mic.h"
#include "net.h"
#include "nn/npu.h"
#include "netcfg.h"
#include "psram.h"
#include "screens/screens.h"
#include "tele.h"
#include "touch.h"
#include "ui/shell.h"

/* "mem": the thread with the least unused stack, idle left out (its margin is
 * small by design and never changes); "stacks": every thread's */
struct stack_scan {
	size_t free;
	char name[16];
	char *list;
	size_t list_len, list_at;
};

static void stack_scan(const struct k_thread *thread, void *data)
{
	struct stack_scan *s = data;
	const char *name = k_thread_name_get((k_tid_t)thread);
	size_t unused;

	if (k_thread_stack_space_get(thread, &unused) != 0) {
		return;
	}
	name = name && name[0] ? name : "?";
	if (s->list) {
		int n = snprintf(s->list + s->list_at, s->list_len - s->list_at, "%s:%u ", name, (unsigned int)unused);

		if (n > 0 && s->list_at + n < s->list_len) {
			s->list_at += n;
		}
	} else if (unused < s->free && strcmp(name, "idle") != 0) {
		s->free = unused;
		snprintf(s->name, sizeof(s->name), "%s", name);
	}
}

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
		shell_costs_reset();
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
	} else if (!strcmp(cmd, "nn")) {
		return npu_ctl(line, out, out_len);
	} else if (!strcmp(cmd, "mic")) {
		struct mic_status m;

		mic_get_status(&m);
		return snprintf(out, out_len,
				"ok=%d total=%u blocks=%u errors=%u last_error=0x%x rate_hz=%u peak=%d rms=%d dc=%d\n", m.ok,
				m.total, m.blocks, m.errors, m.last_error, m.rate_hz, (int)m.peak, (int)m.rms, (int)m.dc);
	} else if (!strcmp(cmd, "sigsrc") && n >= 2) {
		sig_set_source(a[0] == 2 ? SIG_SRC_THEREMIN : a[0] ? SIG_SRC_MIC : SIG_SRC_GENERATOR);
		return snprintf(out, out_len, "source=%d\n", sig_source());
	} else if (!strcmp(cmd, "thm") || !strcmp(cmd, "thmwave") || !strcmp(cmd, "thmsnap") ||
		   !strcmp(cmd, "thmghost") || !strcmp(cmd, "thmsound") || !strcmp(cmd, "thmunder")) {
		/* thm [voice hz db]: a voice by remote (hz 0 lifts it), "thm -1" back to touch;
		 * thmwave <0..3>, thmsnap <0|1>, thmghost <0|1>, thmsound <0|1> (the SOUND
		 * switch), thmunder <dB x10> (its level under the attract lap's music, from
		 * its next flight); each replies with the state */
		struct thm_status st;

		if (!strcmp(cmd, "thm") && n >= 2) {
			theremin_remote(a[0], n >= 3 ? (float)a[1] : 0.0f, n >= 4 ? (float)a[2] : -20.0f);
		} else if (!strcmp(cmd, "thmwave") && n >= 2) {
			theremin_set_wave(a[0]);
		} else if (!strcmp(cmd, "thmsnap") && n >= 2) {
			theremin_set_snap(a[0] != 0);
		} else if (!strcmp(cmd, "thmghost") && n >= 2) {
			theremin_ghost(a[0] != 0);
		} else if (!strcmp(cmd, "thmsound") && n >= 2) {
			theremin_set_sound(a[0] != 0);
		} else if (!strcmp(cmd, "thmunder") && n >= 2) {
			theremin_set_under_db((float)a[0] / 10.0f);
		}
		theremin_get(&st);
		int len = snprintf(out, out_len,
				   "ok enabled=%d mode=%d wave=%d snap=%d sound=%d under=%d under_db10=%d rate_mhz=%u "
				   "blocks=%u synth_us=%u synth_worst_us=%u lat_last_us=%u lat_worst_us=%u lat_n=%u",
				   st.enabled, st.mode, st.wave, st.snap, st.sound, st.under,
				   (int)lroundf(st.under_db * 10.0f), (unsigned int)(st.rate_hz * 1000.0f), st.blocks,
				   st.synth_us, st.synth_worst_us, st.lat_last_us, st.lat_worst_us, st.lat_n);

		for (int v = 0; v < THM_VOICES; v++) {
			len += snprintf(out + len, out_len - len, " v%d=%d/%d/%d/%d", v, st.v[v].on, st.v[v].held,
					(int)(st.v[v].hz + 0.5f), (int)(st.v[v].db - 0.5f));
		}
		return len + snprintf(out + len, out_len - len, "\n");
	} else if (!strncmp(cmd, "mon", 3)) {
		/* the microphone monitor (Signal Lab with MICROPHONE): monlisten <0|1>,
		 * mongain <dB>, moneq <0|1>, monband <band 0..4> <Hz> <dB x10> <Q x100>,
		 * monflat, monreset, montone <Hz> [dBFS x10] (the test tone; 0 ends it);
		 * "mon" and each of them reply with the state */
		struct mon_status st;

		if (!strcmp(cmd, "montone") && n >= 2) {
			monitor_tone((float)a[0], n >= 3 ? (float)a[1] / 10.0f : -20.0f);
		} else if (!strcmp(cmd, "monlisten") && n >= 2) {
			monitor_set_listen(a[0] != 0);
		} else if (!strcmp(cmd, "mongain") && n >= 2) {
			monitor_set_gain(a[0]);
		} else if (!strcmp(cmd, "moneq") && n >= 2) {
			monitor_set_eq(a[0] != 0);
		} else if (!strcmp(cmd, "monband") && n >= 5) {
			monitor_set_band(a[0], (float)a[1], (float)a[2] / 10.0f, (float)a[3] / 100.0f);
		} else if (!strcmp(cmd, "monflat")) {
			monitor_flat();
		} else if (!strcmp(cmd, "monreset")) {
			monitor_reset();
		} else if (strcmp(cmd, "mon")) {
			return snprintf(out, out_len, "err mon, monlisten, mongain, moneq, monband, monflat, monreset\n");
		}
		monitor_get(&st);
		int len = snprintf(out, out_len,
				   "ok enabled=%d listen=%d eq=%d gain_db=%d blocks=%u resyncs=%u lag=%u lag_min=%u "
				   "lag_max=%u delay_us=%u dsp_us=%u dsp_worst_us=%u reduce_db10=%d limited=%u",
				   st.enabled, st.listen, st.eq, st.gain_db, st.blocks, st.resyncs, st.lag,
				   st.lag_min == UINT32_MAX ? 0 : st.lag_min, st.lag_max, st.delay_us, st.dsp_us,
				   st.dsp_worst_us, (int)(st.reduce_db * 10.0f), st.limited);

		for (int b = 0; b < MON_BANDS; b++) {
			len += snprintf(out + len, out_len - len, " b%d=%d/%d/%d", b, (int)(st.band[b].hz + 0.5f),
					(int)lroundf(st.band[b].db * 10.0f), (int)lroundf(st.band[b].q * 100.0f));
		}
		if (st.tone_hz > 0.0f) {
			/* the test tone: in the microphone before and after the equalizer,
			 * the gain measured between them, and the curve's gain there */
			len += snprintf(out + len, out_len - len,
					" tone_hz=%d tone_db10=%d pre_db10=%d post_db10=%d eq_db10=%d curve_db10=%d",
					(int)(st.tone_hz + 0.5f), (int)lroundf(st.tone_db * 10.0f),
					(int)lroundf(st.tone_pre_db * 10.0f), (int)lroundf(st.tone_post_db * 10.0f),
					(int)lroundf((st.tone_post_db - st.tone_pre_db) * 10.0f),
					(int)lroundf((st.eq ? monitor_response_db(&st, st.tone_hz) : 0.0f) * 10.0f));
		}
		return len + snprintf(out + len, out_len - len, "\n");
	} else if (!strcmp(cmd, "micbits") && n >= 4) {
		return mic_bitbang(a[0], a[1], a[2], a[3], out, out_len);
	} else if (!strcmp(cmd, "micscope") && n >= 2) {
		return mic_scope(a[0], out, out_len);
	} else if (!strcmp(cmd, "micprobe")) {
		return mic_probe(out, out_len);
	} else if (!strcmp(cmd, "micpdm") && n >= 2) {
		return mic_pdm(a[0], n >= 3 ? a[1] : 50, n >= 4 ? a[2] : 1, n >= 5 ? a[3] : 768, out, out_len);
	} else if (!strcmp(cmd, "micinject") && n >= 3) {
		return mic_inject(a[0], a[1], n >= 4 ? a[2] : 0, out, out_len);
	} else if (!strcmp(cmd, "mictune")) {
		int v[12], k = sscanf(line, "%*s %d %d %d %d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4],
				      &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]);

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
		return snprintf(out, out_len, "ok=%d blocks=%u block_frames=%u errors=%u uptime_ms=%u\n", audio_ok(), blocks,
				(unsigned int)AUDIO_BLOCK_FRAMES, errors, k_uptime_get_32());
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
	} else if (!strcmp(cmd, "netcfg")) {
		/* "netcfg", "netcfg dhcp", "netcfg static IP MASK [GW]" */
		struct netcfg cfg;
		char mode[8] = "", ip[16] = "", mask[16] = "", gw[16] = "";
		int k = sscanf(line, "%*s %7s %15s %15s %15s", mode, ip, mask, gw);

		if (k >= 1) {
			memset(&cfg, 0, sizeof(cfg));
			cfg.fixed = !strcmp(mode, "static");
			if (!cfg.fixed && strcmp(mode, "dhcp")) {
				return snprintf(out, out_len, "err netcfg dhcp | netcfg static IP MASK [GW]\n");
			}
			strncpy(cfg.ip, ip, sizeof(cfg.ip) - 1);
			strncpy(cfg.mask, mask, sizeof(cfg.mask) - 1);
			strncpy(cfg.gw, gw, sizeof(cfg.gw) - 1);
			const char *why = netcfg_check(&cfg);

			if (why) {
				return snprintf(out, out_len, "err %s\n", why);
			}
			int ret = netcfg_apply(&cfg);

			if (ret) {
				return snprintf(out, out_len, "err %d\n", ret);
			}
		}
		struct net_status ns;

		net_get_status(&ns);
		netcfg_get(&cfg);
		return snprintf(out, out_len, "mode=%s ip=%s mask=%s gw=%s stored=%s recovery=%d store=\"%s\"\n",
				ns.fixed ? "static" : "dhcp", ns.ip, ns.mask, ns.gw, cfg.fixed ? "static" : "dhcp",
				netcfg_recovery(), netcfg_store_state());
	} else if (!strcmp(cmd, "hoodt")) {
		uint32_t t[4];

		hood_timing(t, true);
		return snprintf(out, out_len, "sample_us=%u stack_us=%u net_us=%u side_us=%u\n", t[0], t[1], t[2], t[3]);
	} else if (!strcmp(cmd, "ds")) {
		return ds_info(out, out_len);
	} else if (!strcmp(cmd, "hometiles") && n >= 2) {
		post(CTL_HOME_TILES, a[0]);
	} else if (!strcmp(cmd, "home")) {
		int tiles, scroll, max;

		home_get_status(&tiles, &scroll, &max);
		return snprintf(out, out_len, "tiles=%d scroll=%d max=%d\n", tiles, scroll, max);
	} else if (!strcmp(cmd, "where")) {
		/* which screen is up, whether the demo is running itself, and where its
		 * lap is (ms into the attract lap's music, -1 when the screens take turns) */
		return snprintf(out, out_len, "screen=%d attract=%d lap_ms=%d lap_len=%u\n", shell_current(),
				shell_in_attract(), (int)shell_program_ms(), (unsigned int)shell_program_length_ms());
	} else if (!strcmp(cmd, "mem")) {
		/* for a soak test's leak check: kernel heap and LVGL pool (in use, peak,
		 * size), PSRAM handed out, and the thread with the least stack never used.
		 * The stack scan reads every painted stack (milliseconds): this thread is
		 * below the UI's, so it does not cost a frame. */
		const struct tele *t = tele_get();
		struct stack_scan low = {.free = SIZE_MAX, .name = "-"};

		k_thread_foreach_unlocked(stack_scan, &low);
		return snprintf(out, out_len,
				"heap_used=%u heap_peak=%u heap_size=%u ui_used=%u ui_peak=%u ui_size=%u psram_kb=%u "
				"stack_min_free=%u stack_min_thread=%s uptime_s=%u\n",
				t->heap_used, t->heap_peak, t->heap_size, t->ui_used, t->ui_peak, t->ui_size,
				(unsigned int)(psram_used() / 1024), (unsigned int)low.free, low.name,
				(unsigned int)(k_uptime_get() / 1000));
	} else if (!strcmp(cmd, "stacks")) {
		/* every thread's stack never used, bytes: name:free ... (as many as fit) */
		struct stack_scan all = {.list = out, .list_len = out_len - 1};

		out[0] = '\0';
		k_thread_foreach_unlocked(stack_scan, &all);
		out[all.list_at] = '\n';
		return (int)all.list_at + 1;
	} else if (!strcmp(cmd, "stats")) {
		const struct gfx_stats *g = gfx_stats();

		return snprintf(out, out_len,
				"frames=%u fps=%u.%u render_avg_us=%u render_last_us=%u "
				"render_worst_us=%u period_worst_us=%u tail_worst_us=%u dropped=%u refresh_mhz=%u "
				"margin_min_us=%d late_swaps=%u reader_waits=%u reader_wait_worst_us=%u\n",
				g->frames, g->fps_x10 / 10, g->fps_x10 % 10, g->avg_render_us,
				g->last_render_us, g->worst_render_us, g->worst_period_us, g->worst_tail_us,
				g->dropped, g->refresh_uhz / 1000,
				g->margin_min_us == INT32_MAX ? 0 : (int)g->margin_min_us, g->late_swaps,
				g->reader_waits, g->reader_wait_worst_us);
	} else if (!strcmp(cmd, "rec")) {
		struct rec_status r;

		recorder_get(&r);
		return snprintf(out, out_len,
				"recording=%d pictures=%u missed=%u held=%u copies=%u copy_us=%u copy_worst_us=%u "
				"errors=%u last_error=%d long_waits=%u "
				"encode_us=%u encode_worst_us=%u bytes=%u idr_bytes=%u p_bytes=%u\n",
				r.recording, r.pictures, r.missed, r.held, r.copies, r.copy_us, r.copy_worst_us,
				r.errors, r.last_error, r.long_waits, r.encode_us, r.encode_worst_us, r.bytes,
				r.idr_bytes, r.p_bytes);
	} else if (!strcmp(cmd, "svct")) {
		int k = shell_costs(out, out_len - 1);

		out[k] = '\n';
		return k + 1;
	} else if (!strcmp(cmd, "swtrace") || !strcmp(cmd, "droptrace")) {
		/* the frames of the last screen transition, or before the last
		 * dropped frame, each as wake/tail/pre/lv/away/left/alpha in us
		 * (gfx.h); "u" at the end: the frame before was not presented, so no
		 * deadline applied */
		const struct gfx_trace *t = gfx_trace();
		const struct gfx_drop *d = gfx_last_drop();
		const struct gfx_trace_frame *fr;
		int nf, k;

		if (cmd[0] == 's') {
			k = snprintf(out, out_len, "seq=%u from=%d to=%d at_switch=%u switch_us=%u n=%u f=", t->seq,
				     t->from, t->to, t->at_switch, t->switch_us, t->n);
			fr = t->f;
			nf = t->n;
		} else {
			k = snprintf(out, out_len, "frame=%u seq=%u screen=%d f=", d->frame, d->seq, d->screen);
			fr = d->f;
			nf = d->frame ? GFX_DROP_N : 0;
		}
		for (int i = 0; i < nf && k < (int)out_len - 46; i++) {
			const struct gfx_trace_frame *f = &fr[i];

			k += snprintf(out + k, out_len - k, "%s%u/%u/%u/%u/%u/%d/%u%s", i ? "," : "", f->wake_us,
				      f->tail_us, f->pre_us, f->lv_us, f->away_us, f->left_us, f->alpha,
				      f->locked ? "" : "u");
		}
		return k + snprintf(out + k, out_len - k, "\n");
	} else {
		return snprintf(out, out_len, "err unknown: %s\n", cmd);
	}
	return snprintf(out, out_len, "ok\n");
}

static void ctl_thread(void *p1, void *p2, void *p3)
{
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(CTL_PORT)};
	static char line[96], out[1024];
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
