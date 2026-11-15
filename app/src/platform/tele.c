/*
 * Telemetry sampler.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>

#include <lvgl.h>
#include <lvgl_mem.h>

#include "net.h"
#include "tele.h"

#define MAX_TRACKED 24

extern struct k_heap _system_heap;

struct tracked {
	const struct k_thread *tid;
	uint64_t prev;
	uint64_t delta;
	bool seen;
};

static struct tele tele;
static struct tracked tracked[MAX_TRACKED];
static uint64_t prev_all, prev_busy;
static uint64_t prev_rx, prev_tx;
static uint32_t last_ms;

static void thread_cb(const struct k_thread *thread, void *user_data)
{
	k_thread_runtime_stats_t rs;
	struct tracked *slot = NULL;

	ARG_UNUSED(user_data);
	if (k_thread_runtime_stats_get((k_tid_t)thread, &rs) != 0) {
		return;
	}
	for (int i = 0; i < MAX_TRACKED; i++) {
		if (tracked[i].tid == thread) {
			slot = &tracked[i];
			break;
		}
		if (slot == NULL && tracked[i].tid == NULL) {
			slot = &tracked[i];
		}
	}
	if (slot == NULL) {
		return;
	}
	if (slot->tid != thread) {
		slot->tid = thread;
		slot->prev = rs.execution_cycles;
	}
	slot->delta = rs.execution_cycles - slot->prev;
	slot->prev = rs.execution_cycles;
	slot->seen = true;
}

static void sample_cpu(void)
{
	k_thread_runtime_stats_t all;
	uint64_t d_all, d_busy;

	if (k_thread_runtime_stats_all_get(&all) != 0) {
		return;
	}
	d_all = all.execution_cycles - prev_all;
	d_busy = all.total_cycles - prev_busy;
	prev_all = all.execution_cycles;
	prev_busy = all.total_cycles;
	if (d_all == 0) {
		return;
	}
	tele.cpu_pct_x10 = (uint16_t)MIN(d_busy * 1000 / d_all, 1000);

	for (int i = 0; i < MAX_TRACKED; i++) {
		tracked[i].seen = false;
	}
	k_thread_foreach(thread_cb, NULL);

	tele.n_thr = 0;
	for (int i = 0; i < MAX_TRACKED; i++) {
		if (!tracked[i].seen) {
			tracked[i].tid = NULL; /* thread exited */
			continue;
		}
		const char *name = k_thread_name_get((k_tid_t)tracked[i].tid);

		if (name == NULL || strcmp(name, "idle") == 0) {
			continue;
		}
		uint16_t pct = (uint16_t)MIN(tracked[i].delta * 1000 / d_all, 1000);
		int pos = tele.n_thr;

		/* insertion sort, busiest first, keep the top TELE_THREADS */
		while (pos > 0 && tele.thr[pos - 1].pct_x10 < pct) {
			pos--;
		}
		if (pos >= TELE_THREADS) {
			continue;
		}
		int last = MIN(tele.n_thr, TELE_THREADS - 1);

		for (int j = last; j > pos; j--) {
			tele.thr[j] = tele.thr[j - 1];
		}
		strncpy(tele.thr[pos].name, name, sizeof(tele.thr[pos].name) - 1);
		tele.thr[pos].name[sizeof(tele.thr[pos].name) - 1] = '\0';
		tele.thr[pos].pct_x10 = pct;
		if (tele.n_thr < TELE_THREADS) {
			tele.n_thr++;
		}
	}
}

static void sample_heaps(void)
{
	struct sys_memory_stats ms;

	if (sys_heap_runtime_stats_get(&_system_heap.heap, &ms) == 0) {
		tele.heap_used = ms.allocated_bytes;
		tele.heap_peak = ms.max_allocated_bytes;
		tele.heap_size = ms.allocated_bytes + ms.free_bytes;
	}
	lvgl_heap_stats(&ms);
	tele.ui_used = ms.allocated_bytes;
	tele.ui_peak = ms.max_allocated_bytes;
	tele.ui_size = ms.allocated_bytes + ms.free_bytes;
}

static void sample_net(uint32_t dt_ms)
{
	struct net_status ns;

	net_get_status(&ns);
	if (dt_ms) {
		tele.rx_bps = (uint32_t)((ns.rx_bytes - prev_rx) * 1000 / dt_ms);
		tele.tx_bps = (uint32_t)((ns.tx_bytes - prev_tx) * 1000 / dt_ms);
	}
	prev_rx = ns.rx_bytes;
	prev_tx = ns.tx_bytes;
}

void tele_poll(uint32_t now_ms)
{
	uint32_t dt = now_ms - last_ms;

	if (dt < TELE_PERIOD_MS) {
		return;
	}
	last_ms = now_ms;
	sample_cpu();
	sample_heaps();
	sample_net(dt);

	tele.cpu_ring[tele.head] = tele.cpu_pct_x10;
	tele.rx_ring[tele.head] = tele.rx_bps;
	tele.tx_ring[tele.head] = tele.tx_bps;
	tele.head = (tele.head + 1) % TELE_HISTORY;
	tele.samples++;
}

const struct tele *tele_get(void)
{
	return &tele;
}
