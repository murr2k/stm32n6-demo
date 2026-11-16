/*
 * Host link: telemetry pushed by the companion program on a PC
 * (host/companion.py) as UDP datagrams, about ten a second.
 *
 * Datagram to the board, udp/5557, little-endian:
 *   "N6HL", u32 seq, u32 rtt_us, u32 rx_bps, u32 tx_bps, u16 cpu_x10,
 *   u8 mem_pct, u8 n_cores, n_cores x u8 load (0..100)
 *   optionally, the chip's topology: n_cores x u8 kind, n_cores x u8 core
 *     kind: bits 0-1 the core type (HL_CORE_*), bits 2-3 the SMT thread
 *     core: index of the physical core this logical processor belongs to
 * The board answers each one with "N6HR", u32 seq so the host can time the
 * round trip; the host reports the previous round trip in rtt_us. n_cores is
 * a byte, so a host can describe up to 255 logical processors; the board
 * keeps HL_MAX_CORES of them.
 *
 * Discovery: a datagram "N6HQ" to udp/5557, usually broadcast, is answered
 * (to the sender) with "N6HA" and one line of text: what the board is and
 * where its services are, e.g.
 *   N6HA N6DK board=STM32N6570-DK ip=192.168.254.86 mac=02:80:e1:4e:36:01
 *        hostlink=5557 ctl=5556 shot=5555 media=5558 http=80 fw=<build>
 * The host companion uses it to find boards without being told an address;
 * the answer to a broadcast passes Windows' firewall as a reply.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_HOSTLINK_H
#define PLATFORM_HOSTLINK_H

#include <stdbool.h>
#include <stdint.h>

#define HL_MAX_CORES 128
#define HL_HISTORY   250

/* Core types of a hybrid CPU. HL_CORE_PLAIN: not hybrid, or not known. */
#define HL_CORE_PLAIN 0
#define HL_CORE_P     1
#define HL_CORE_E     2
#define HL_CORE_LPE   3 /* low-power E-core, outside the shared L3 */
#define HL_KIND_TYPE(k)   ((k) & 3)
#define HL_KIND_THREAD(k) (((k) >> 2) & 3)

struct hl_state {
	uint32_t last_ms;       /* k_uptime of the last datagram, 0 if none yet */
	char host_ip[16];
	uint32_t packets, lost; /* lost: gaps in the sequence numbers */
	uint32_t rtt_us;        /* as measured by the host */
	uint32_t rx_bps, tx_bps;/* the host's own network throughput, bytes/s */
	uint16_t cpu_x10;
	uint8_t mem_pct, n_cores;  /* n_cores: the ones kept, at most HL_MAX_CORES */
	uint8_t n_host_cores;      /* what the host has (up to 255) */
	bool topo;              /* core_kind and core_id are from the host */
	uint8_t core_kind[HL_MAX_CORES];
	uint8_t core_id[HL_MAX_CORES];
	uint8_t load[HL_MAX_CORES]; /* the latest sample, 0..100 */
	uint16_t head;          /* next write index of the histories */
	uint8_t (*core_hist)[HL_MAX_CORES]; /* HL_HISTORY rows, in PSRAM (32 KB) */
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
