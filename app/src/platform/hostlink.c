/*
 * Host link receiver. See hostlink.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>

#include "ctl.h"
#include "hostlink.h"
#include "net.h"
#include "psram.h"

LOG_MODULE_REGISTER(hostlink, LOG_LEVEL_INF);

#define HL_PORT 5557
#define HL_HDR  24

/* The profiler's trace port, in the discovery answer of a zprof build only
 * (zprof's bridge finds the board by it); the release build's answer is unchanged. */
#ifdef CONFIG_ZPROF
#define HL_PROF " prof=" STRINGIFY(CONFIG_ZPROF_TCP_PORT)
#else
#define HL_PROF ""
#endif

static struct hl_state st;
static uint32_t last_seq;

const struct hl_state *hostlink_state(void)
{
	return &st;
}

bool hostlink_online(void)
{
	/* signed: the receiver may stamp last_ms after this thread read the clock */
	return st.last_ms != 0 && (int32_t)(k_uptime_get_32() - st.last_ms) < 2000;
}

static void hostlink_thread(void *p1, void *p2, void *p3)
{
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(HL_PORT)};
	static uint8_t buf[HL_HDR + 3 * 255 + 8];
	int s;

	st.core_hist = psram_alloc(HL_HISTORY * HL_MAX_CORES, 32);
	if (st.core_hist == NULL) {
		LOG_ERR("no memory for the history");
		return;
	}
	memset(st.core_hist, 0, HL_HISTORY * HL_MAX_CORES);
	net_wait_ip();
	s = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s < 0 || zsock_bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("socket setup failed (%d)", errno);
		return;
	}
	LOG_INF("host link on udp/%d", HL_PORT);
	for (;;) {
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		ssize_t n = zsock_recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);

		if (n >= 4 && memcmp(buf, "N6HQ", 4) == 0) {
			static char answer[200];
			struct net_status ns;
			struct net_linkaddr *mac = net_if_get_link_addr(net_if_get_default());

			net_get_status(&ns);
			int len = snprintf(answer, sizeof(answer),
					   "N6HA N6DK board=STM32N6570-DK ip=%s mac=%02x:%02x:%02x:%02x:%02x:%02x "
					   "hostlink=%d ctl=5556 shot=5555 media=5558 http=80" HL_PROF " fw=%s %s",
					   ns.ip, mac->addr[0], mac->addr[1], mac->addr[2], mac->addr[3], mac->addr[4],
					   mac->addr[5], HL_PORT, __DATE__, __TIME__);

			zsock_sendto(s, answer, MIN(len, (int)sizeof(answer) - 1), 0, (struct sockaddr *)&from, from_len);
			continue;
		}
		if (n < HL_HDR || memcmp(buf, "N6HL", 4) != 0) {
			continue;
		}
		uint32_t seq = sys_get_le32(buf + 4);
		uint8_t cores = MIN(buf[23], HL_MAX_CORES);

		if (n < HL_HDR + cores) {
			continue;
		}
		/* answer first: the reply is what the host times. When the web remote
		 * has asked for a tour, one reply (to one companion) carries the
		 * request's number after the sequence number (ctl.h). */
		uint8_t reply[12] = {'N', '6', 'H', 'R'};
		uint32_t tour = ctl_tour_take_request();

		sys_put_le32(seq, reply + 4);
		sys_put_le32(tour, reply + 8);
		zsock_sendto(s, reply, tour ? 12 : 8, 0, (struct sockaddr *)&from, from_len);
		if (tour) {
			char ip[NET_IPV4_ADDR_LEN];

			net_addr_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
			LOG_INF("tour %u asked of the companion at %s", (unsigned int)tour, ip);
		}

		if (st.packets > 0 && seq > last_seq + 1) {
			st.lost += seq - last_seq - 1;
		} else if (seq < last_seq) {
			st.lost = 0; /* the companion restarted */
		}
		last_seq = seq;
		st.packets++;
		st.rtt_us = sys_get_le32(buf + 8);
		st.rx_bps = sys_get_le32(buf + 12);
		st.tx_bps = sys_get_le32(buf + 16);
		st.cpu_x10 = sys_get_le16(buf + 20);
		st.mem_pct = buf[22];
		st.n_cores = cores;
		st.n_host_cores = buf[23];
		net_addr_ntop(AF_INET, &from.sin_addr, st.host_ip, sizeof(st.host_ip));

		/* the topology follows the loads when the host knows it (and all of it fits) */
		if (buf[23] == cores && n >= HL_HDR + 3 * cores) {
			memcpy(st.core_kind, buf + HL_HDR + cores, cores);
			memcpy(st.core_id, buf + HL_HDR + 2 * cores, cores);
			st.topo = true;
		} else {
			st.topo = false;
		}
		for (int i = 0; i < cores; i++) {
			st.load[i] = MIN(buf[HL_HDR + i], 100);
		}

		uint16_t h = st.head;

		memset(st.core_hist[h], 0, HL_MAX_CORES);
		memcpy(st.core_hist[h], buf + HL_HDR, cores);
		st.cpu_hist[h] = st.cpu_x10;
		st.rtt_hist[h] = st.rtt_us;
		st.head = (h + 1) % HL_HISTORY;
		st.last_ms = k_uptime_get_32();
	}
}
K_THREAD_DEFINE(hostlink, 3072, hostlink_thread, NULL, NULL, NULL, 8, 0, 0);
