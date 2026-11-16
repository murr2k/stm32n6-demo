/*
 * Board address setting. See netcfg.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_if.h>

#include "media/sdcard.h"
#include "net.h"
#include "netcfg.h"

LOG_MODULE_REGISTER(netcfg, LOG_LEVEL_INF);

#define CFG_PATH "/SD:/n6demo.cfg"

static struct netcfg cur;
static bool recovery;
static volatile const char *store_state = "not changed";
static K_SEM_DEFINE(save_req, 0, 1);
static K_MUTEX_DEFINE(lock);

static const struct gpio_dt_spec user_btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static bool parse(const char *s, struct net_in_addr *a)
{
	return s[0] && net_addr_pton(AF_INET, s, a) == 0;
}

const char *netcfg_check(const struct netcfg *cfg)
{
	struct net_in_addr ip, mask, gw;

	if (!cfg->fixed) {
		return NULL;
	}
	if (!parse(cfg->ip, &ip) || ip.s4_addr[0] == 0 || ip.s4_addr[0] >= 224 || ip.s4_addr[0] == 127) {
		return "the address is not a usable IPv4 address";
	}
	if (!parse(cfg->mask, &mask)) {
		return "the mask is not an IPv4 address";
	}
	uint32_t m = ntohl(mask.s_addr);

	if (m == 0 || (~m & (~m + 1)) != 0) { /* ones followed by zeros */
		return "the mask is not a run of ones then zeros";
	}
	if ((ntohl(ip.s_addr) & ~m) == 0 || (ntohl(ip.s_addr) & ~m) == ~m) {
		return "the address is the network or broadcast address";
	}
	if (cfg->gw[0]) {
		if (!parse(cfg->gw, &gw)) {
			return "the gateway is not an IPv4 address";
		}
		if ((ntohl(gw.s_addr) & m) != (ntohl(ip.s_addr) & m)) {
			return "the gateway is not on the board's subnet";
		}
	}
	return NULL;
}

void netcfg_load(struct netcfg *cfg)
{
	struct fs_file_t f;
	char buf[160] = {0};

	memset(cfg, 0, sizeof(*cfg));
	if (gpio_is_ready_dt(&user_btn) && gpio_pin_configure_dt(&user_btn, GPIO_INPUT) == 0 &&
	    gpio_pin_get_dt(&user_btn) > 0) {
		recovery = true;
		LOG_WRN("USER button held: DHCP, the stored address is ignored");
	}
	if (sdcard_mount() == 0) {
		struct fs_dirent ent;

		fs_file_t_init(&f);
		/* ask first: the fs layer logs an error for every failed open */
		if (fs_stat(CFG_PATH, &ent) == 0 && fs_open(&f, CFG_PATH, FS_O_READ) == 0) {
			fs_read(&f, buf, sizeof(buf) - 1);
			fs_close(&f);
		}
	}
	/* "mode=static ip=a.b.c.d mask=... gw=..." on one line */
	char mode[8] = "";

	for (char *t = strtok(buf, " \r\n"); t; t = strtok(NULL, " \r\n")) {
		if (sscanf(t, "mode=%7s", mode) == 1) {
			continue;
		}
		if (strncmp(t, "ip=", 3) == 0) {
			strncpy(cfg->ip, t + 3, sizeof(cfg->ip) - 1);
		} else if (strncmp(t, "mask=", 5) == 0) {
			strncpy(cfg->mask, t + 5, sizeof(cfg->mask) - 1);
		} else if (strncmp(t, "gw=", 3) == 0) {
			strncpy(cfg->gw, t + 3, sizeof(cfg->gw) - 1);
		}
	}
	cfg->fixed = strcmp(mode, "static") == 0;
	if (cfg->fixed && netcfg_check(cfg) != NULL) {
		LOG_WRN("stored address not usable (%s): DHCP", netcfg_check(cfg));
		cfg->fixed = false;
	}
	cur = *cfg;
	if (recovery) {
		cfg->fixed = false;
	}
}

bool netcfg_recovery(void)
{
	return recovery;
}

void netcfg_get(struct netcfg *cfg)
{
	k_mutex_lock(&lock, K_FOREVER);
	*cfg = cur;
	k_mutex_unlock(&lock);
}

const char *netcfg_store_state(void)
{
	return (const char *)store_state;
}

int netcfg_apply(const struct netcfg *cfg)
{
	if (netcfg_check(cfg) != NULL) {
		return -EINVAL;
	}
	/* the switch waits for DHCP to let go (up to a second): not on the caller's thread */
	k_mutex_lock(&lock, K_FOREVER);
	cur = *cfg;
	k_mutex_unlock(&lock);
	store_state = "applying";
	k_sem_give(&save_req);
	return 0;
}

/* Applying and the card write, away from the UI thread (no card I/O there,
 * and the switch may wait for DHCP).
 */
static void save_thread(void *a, void *b, void *c)
{
	for (;;) {
		struct netcfg cfg;
		struct fs_file_t f;
		char line[128];

		k_sem_take(&save_req, K_FOREVER);
		netcfg_get(&cfg);
		if (net_use_config(&cfg) != 0) {
			store_state = "could not apply";
			continue;
		}
		store_state = "saving";
		int len = snprintf(line, sizeof(line), "mode=%s ip=%s mask=%s gw=%s\n", cfg.fixed ? "static" : "dhcp",
				   cfg.ip, cfg.mask, cfg.gw);

		if (sdcard_mount() != 0) {
			store_state = "no card: kept until power-off";
			continue;
		}
		fs_file_t_init(&f);
		if (fs_open(&f, CFG_PATH, FS_O_CREATE | FS_O_WRITE) == 0) {
			fs_truncate(&f, 0);
			int w = fs_write(&f, line, len);

			fs_close(&f);
			store_state = w == len ? "saved on the card" : "card write failed";
		} else {
			store_state = "card write failed";
		}
		LOG_INF("address setting: %s (%s)", line, (const char *)store_state);
	}
}
K_THREAD_DEFINE(netcfg_save, 3072, save_thread, NULL, NULL, NULL, 11, 0, 0);
