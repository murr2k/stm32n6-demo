/*
 * Network platform.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_stats.h>
#include <zephyr/net/socket.h>

#include "gfx.h"
#include "net.h"

LOG_MODULE_REGISTER(net, LOG_LEVEL_INF);

#define FBDUMP_PORT 5555

static struct net_mgmt_event_callback ipv4_cb;
static struct net_mgmt_event_callback link_cb;
static struct net_status status;
static struct k_spinlock lock;
static K_SEM_DEFINE(ip_ready, 0, 1);

static void ipv4_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	if (event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}
	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *a = &iface->config.ip.ipv4->unicast[i].ipv4;

		if (a->is_used && a->addr_type == NET_ADDR_DHCP) {
			char buf[NET_IPV4_ADDR_LEN];
			k_spinlock_key_t key;

			net_addr_ntop(AF_INET, &a->address.in_addr, buf, sizeof(buf));
			key = k_spin_lock(&lock);
			strncpy(status.ip, buf, sizeof(status.ip) - 1);
			status.have_ip = true;
			k_spin_unlock(&lock, key);
			LOG_INF("ip %s", buf);
			k_sem_give(&ip_ready);
		}
	}
}

static void link_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (event == NET_EVENT_IF_UP) {
		status.link_up = true;
	} else if (event == NET_EVENT_IF_DOWN) {
		status.link_up = false;
		status.have_ip = false;
		status.ip[0] = '\0';
	}
	k_spin_unlock(&lock, key);
}

int net_start(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		return -ENODEV;
	}
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);
	net_mgmt_init_event_callback(&link_cb, link_event, NET_EVENT_IF_UP | NET_EVENT_IF_DOWN);
	net_mgmt_add_event_callback(&link_cb);
	status.link_up = net_if_is_up(iface);
	net_dhcpv4_start(iface);
	return 0;
}

void net_get_status(struct net_status *out)
{
	struct net_stats_eth eth = {0};
	k_spinlock_key_t key;

	if (net_mgmt(NET_REQUEST_STATS_GET_ETHERNET, net_if_get_default(), &eth, sizeof(eth)) == 0) {
		key = k_spin_lock(&lock);
		status.rx_bytes = eth.bytes.received;
		status.tx_bytes = eth.bytes.sent;
		k_spin_unlock(&lock, key);
	}
	key = k_spin_lock(&lock);
	*out = status;
	k_spin_unlock(&lock, key);
}

void net_wait_ip(void)
{
	k_sem_take(&ip_ready, K_FOREVER);
	k_sem_give(&ip_ready);
}

/* Frame buffer dump: connect to tcp/5555, receive a 16-byte header then one
 * frame captured at a frame boundary. tools/shot.py turns it into a PNG.
 */
static void fbdump_thread(void *a, void *b, void *c)
{
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(FBDUMP_PORT)};
	int srv;

	net_wait_ip();
	srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (srv < 0 || zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    zsock_listen(srv, 1) < 0) {
		LOG_ERR("fbdump: socket setup failed (%d)", errno);
		return;
	}
	LOG_INF("fbdump on tcp/%d", FBDUMP_PORT);
	for (;;) {
		int s = zsock_accept(srv, NULL, NULL);

		if (s < 0) {
			continue;
		}
		const uint8_t *p = gfx_snapshot();
		const uint32_t hdr[4] = {0x4246364e /* "N6FB" */, GFX_W, GFX_H, 32};
		size_t left = GFX_FB_BYTES;

		zsock_send(s, hdr, sizeof(hdr), 0);
		while (left) {
			ssize_t n = zsock_send(s, p, MIN(left, 16384), 0);

			if (n <= 0) {
				break;
			}
			p += n;
			left -= n;
		}
		zsock_close(s);
	}
}
K_THREAD_DEFINE(fbdump, 4096, fbdump_thread, NULL, NULL, NULL, 10, 0, 0);
