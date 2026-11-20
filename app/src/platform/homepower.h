/*
 * Home power: the household's electricity, from the user's energy monitor
 * over the internet (its web API: plain HTTP, JSON). The monitor's address is
 * set on the board (ctl "power url http://HOST[:PORT]") and kept on the card
 * in /SD:/power.cfg, never in this repository.
 *
 * A thread of its own resolves the name and polls /api/stats (the power now,
 * the last 24 hours, the billing period) and /api/dashboard?range= (the
 * chart's series). Nothing on the UI thread waits for the network: readers
 * copy the small figures, and read a series from the copy published last
 * (two per series, swapped when a new one is complete; the next one is
 * written 15 s or more later).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_HOMEPOWER_H
#define PLATFORM_HOMEPOWER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum hp_range { HP_1H, HP_6H, HP_24H, HP_7D, HP_30D, HP_RANGES };

/* "1h", "6h", "24h", "7d", "30d" */
const char *homepower_range_name(int range);

/* From /api/stats. */
struct hp_stats {
	uint32_t seq;        /* 0 until the first reading; changes with every one */
	uint32_t fetched_ms; /* k_uptime_get_32() when it arrived */
	int32_t age_s;       /* the reading's age then, by the server's clock; -1 unknown */
	float now_w, min_w, avg_w, max_w, cost_24h;
	bool have_bill, have_trend;
	float bill_total;    /* the bill so far, all charges */
	float slope_per_day; /* spend rate */
	float projected;     /* the projected bill for the period */
	float energy_kwh;
	int cycle_days, days;
};

/* From /api/dashboard: per bucket the mean, minimum and maximum (W), and the
 * range's own figures. */
#define HP_MAX_POINTS 1024
struct hp_series {
	uint32_t seq;
	uint8_t range;
	int n;
	int64_t start_ms, end_ms; /* the window, server clock (epoch ms) */
	float min_w, mean_w, max_w, energy_wh, meter_kwh, rate, cost_hour, est_cost;
	int32_t t_s[HP_MAX_POINTS]; /* seconds after start_ms */
	float avg[HP_MAX_POINTS], lo[HP_MAX_POINTS], hi[HP_MAX_POINTS];
};

/* From main(), once: its buffers and stack go in PSRAM. */
void homepower_start(void);

/* The monitor answered the last request (or the one before): the panel's
 * tile shows on Home. */
bool homepower_online(void);
bool homepower_configured(void);
/* What the feed is doing or what went wrong, for the screen and ctl. */
const char *homepower_state(void);

void homepower_get_stats(struct hp_stats *out);
/* The panel's chart (its range); NULL before the first. */
const struct hp_series *homepower_series(void);

/* The panel shows: poll every 15 s instead of every minute, and fetch its
 * chart. Wakes the thread at once. */
void homepower_set_visible(bool on);
/* Home shows, with the tile's live gauge: the figures every 15 s too. */
void homepower_set_glance(bool on);
void homepower_set_range(int range);
int homepower_range(void);

/* ctl "power": state and counters. */
int homepower_status(char *out, size_t len);
/* ctl "power url ...": "http://host[:port][/prefix]" or "none". Applied at
 * once and saved on the card by the feed's thread. Returns 0 or -EINVAL. */
int homepower_set_url(const char *url);
/* ctl "power url": the address in use (not shown anywhere else). */
int homepower_get_url(char *out, size_t len);

#endif
