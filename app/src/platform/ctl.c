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

#include "ctl.h"
#include "gfx.h"
#include "net.h"
#include "touch.h"

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
	static char line[96], out[256];
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
