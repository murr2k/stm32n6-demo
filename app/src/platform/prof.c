/* zprof's port hooks and the demo's metrics, in a zprof build only (build-zprof:
 * the module and CONFIG_ZPROF come from the command line).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef CONFIG_ZPROF

#include <zephyr/kernel.h>
#include <zprof/zprof.h>

#include "audio.h"
#include "gfx.h"
#include "psram.h"

/* zprof's capture buffer (CONFIG_ZPROF_CAPTURE_KB), taken at the first capture and kept */
void *zprof_port_alloc(size_t bytes)
{
	return psram_alloc(bytes, 32);
}

/* The demo's own counters, as zprof metrics at every heartbeat: the host checks its
 * trace against them (frames, drops, the DAC's blocks and restarts). */
ZPROF_METRIC_DEFINE(m_frames, "gfx.frames", GAUGE, "frames");
ZPROF_METRIC_DEFINE(m_dropped, "gfx.dropped", GAUGE, "frames");
ZPROF_METRIC_DEFINE(m_period, "gfx.period_us", GAUGE, "us");          /* the panel's refresh */
ZPROF_METRIC_DEFINE(m_margin_min, "gfx.margin_min_us", GAUGE, "us");  /* since ctl "reset" */
ZPROF_METRIC_DEFINE(m_audio_blocks, "audio.blocks", GAUGE, "blocks");
ZPROF_METRIC_DEFINE(m_audio_errors, "audio.underruns", GAUGE, "restarts");
ZPROF_METRIC_DEFINE(m_psram, "psram.used", GAUGE, "KB");

void zprof_port_heartbeat(void)
{
	const struct gfx_stats *g = gfx_stats();
	uint32_t blocks, errors;

	ZPROF_SET(m_frames, g->frames);
	ZPROF_SET(m_dropped, g->dropped);
	ZPROF_SET(m_period, g->period_us);
	ZPROF_SET(m_margin_min, g->margin_min_us);
	audio_get_counters(&blocks, &errors);
	ZPROF_SET(m_audio_blocks, blocks);
	ZPROF_SET(m_audio_errors, errors);
	ZPROF_SET(m_psram, psram_used() / 1024u);
}

#endif /* CONFIG_ZPROF */
