/*
 * Host link: telemetry pushed by the companion program on a PC
 * (host/companion.py) as UDP datagrams, about ten a second.
 *
 * Datagram to the board, udp/5557, little-endian:
 *   "N6HL", u32 seq, u32 rtt_us, u32 rx_bps, u32 tx_bps, u16 cpu_x10,
 *   u8 mem_pct, u8 n_cores, n_cores x u8 load (0..100)
 * The board answers each one with "N6HR", u32 seq so the host can time the
 * round trip; the host reports the previous round trip in rtt_us.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_HOSTLINK_H
#define PLATFORM_HOSTLINK_H

#include <stdbool.h>
#include <stdint.h>

#define HL_MAX_CORES 32
#define HL_HISTORY   250

struct hl_state {
	uint32_t last_ms;       /* k_uptime of the last datagram, 0 if none yet */
	char host_ip[16];
	uint32_t packets, lost; /* lost: gaps in the sequence numbers */
	uint32_t rtt_us;        /* as measured by the host */
	uint32_t rx_bps, tx_bps;/* the host's own network throughput, bytes/s */
	uint16_t cpu_x10;
	uint8_t mem_pct, n_cores;
	uint16_t head;          /* next write index of the histories */
	uint8_t core_hist[HL_HISTORY][HL_MAX_CORES];
	uint16_t cpu_hist[HL_HISTORY];
	uint32_t rtt_hist[HL_HISTORY];
};

/* Written by the receiver thread, read by the UI: fields may be one sample
 * apart from each other, which does not matter for display.
 */
const struct hl_state *hostlink_state(void);

/* True if a datagram arrived within the last two seconds. */
bool hostlink_online(void);

#endif
