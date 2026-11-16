/*
 * Network platform: link and address state, frame buffer dump service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_NET_H
#define PLATFORM_NET_H

#include <stdbool.h>
#include <stdint.h>

struct net_status {
	bool link_up;
	bool have_ip;
	char ip[16];          /* dotted quad, empty until a lease arrives or a fixed address is set */
	char mask[16], gw[16];
	bool fixed;           /* a fixed address, not DHCP */
	uint64_t rx_bytes;
	uint64_t tx_bytes;
};

struct netcfg;

/* Starts with the stored address setting (netcfg.h): DHCP or a fixed address. */
int net_start(void);
/* Switch to cfg now, without a restart. Connections on the old address end. */
int net_use_config(const struct netcfg *cfg);
void net_get_status(struct net_status *out);

/* Blocks until an IPv4 address is configured. */
void net_wait_ip(void);

#endif
