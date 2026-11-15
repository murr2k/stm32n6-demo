/*
 * Telemetry: what the board knows about itself. Everything here is measured,
 * nothing is simulated.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_TELE_H
#define PLATFORM_TELE_H

#include <stdbool.h>
#include <stdint.h>

#define TELE_THREADS 8
#define TELE_HISTORY 180   /* samples at TELE_PERIOD_MS */
#define TELE_PERIOD_MS 250

struct tele_thread {
	char name[16];
	uint16_t pct_x10;       /* share of all CPU time in the last period */
};

struct tele {
	uint32_t samples;
	uint16_t cpu_pct_x10;   /* non-idle CPU time */
	struct tele_thread thr[TELE_THREADS]; /* busiest first, idle excluded */
	int n_thr;
	uint32_t heap_used, heap_peak, heap_size;     /* kernel heap, bytes */
	uint32_t ui_used, ui_peak, ui_size;           /* LVGL heap, bytes */
	uint32_t rx_bps, tx_bps;                      /* Ethernet payload bytes per second */
	uint16_t head;                                /* next write index of the rings */
	uint16_t cpu_ring[TELE_HISTORY];
	uint32_t rx_ring[TELE_HISTORY];
	uint32_t tx_ring[TELE_HISTORY];
};

/* Call every frame slot from the UI thread; samples every TELE_PERIOD_MS. */
void tele_poll(uint32_t now_ms);
const struct tele *tele_get(void);

#endif
