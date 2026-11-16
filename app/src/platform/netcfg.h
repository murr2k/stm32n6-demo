/*
 * The board's own address: DHCP (the default) or a fixed address set on the
 * Settings screen or with ctl "netcfg". Kept in /SD:/n6demo.cfg on the
 * microSD card; without a card the setting lasts until power-off.
 *
 * Holding the USER button while the board starts ignores a fixed address
 * and uses DHCP, so a wrong one cannot lock the board out of the network.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_NETCFG_H
#define PLATFORM_NETCFG_H

#include <stdbool.h>
#include <stddef.h>

struct netcfg {
	bool fixed;   /* false: DHCP */
	char ip[16];
	char mask[16];
	char gw[16];
};

/* The stored setting, read once at start-up (main thread, before the UI). */
void netcfg_load(struct netcfg *cfg);
/* True if the USER button was held at start-up (DHCP forced). */
bool netcfg_recovery(void);

/* Check the three addresses of a fixed setting; returns NULL or what is wrong. */
const char *netcfg_check(const struct netcfg *cfg);

/* Use cfg from now on, without a restart, and store it. Both happen on a
 * background thread within about a second; netcfg_store_state() follows it.
 * Returns 0, or -EINVAL if cfg does not pass netcfg_check().
 */
int netcfg_apply(const struct netcfg *cfg);

/* What is stored (or about to be). */
void netcfg_get(struct netcfg *cfg);
/* "saved", "saving", "no card: not kept after power-off", ... */
const char *netcfg_store_state(void);

#endif
