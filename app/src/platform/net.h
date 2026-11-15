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
	char ip[16];          /* dotted quad, empty until a lease arrives */
	uint64_t rx_bytes;
	uint64_t tx_bytes;
};

int net_start(void);
void net_get_status(struct net_status *out);

/* Blocks until an IPv4 address is configured. */
void net_wait_ip(void);

#endif
