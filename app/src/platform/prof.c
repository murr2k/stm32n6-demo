/* zprof's port hooks and the demo's metrics, in a zprof build only (build-zprof:
 * the module and CONFIG_ZPROF come from the command line).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef CONFIG_ZPROF

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zprof/zprof.h>

#ifdef CONFIG_ZPROF_SAMPLE
#include <zephyr/linker/section_tags.h>
#include <soc.h>
#include <stm32n6xx_hal.h>
#endif

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

#ifdef CONFIG_ZPROF_SAMPLE
/*
 * zprof's PC sampling timer: TIM7, a basic timer nothing else here uses (mic.c has
 * TIM6), counting microseconds, its update interrupt straight into zprof's handler
 * (a direct vector, priority 0: under irq_lock() like every zprof writer). Each
 * period is the nominal one give or take an eighth, from a small LFSR, so samples
 * cannot fall into step with the 60 Hz frames or the 10 kHz kernel tick. Only
 * while a zprof client asks for samples; stopped when it leaves.
 */
static uint16_t arr_base, arr_jit;
static uint32_t lfsr = 0xACE1u;

/* TIM7's kernel clock, counted (divided by 8) against the core's cycle counter over
 * 100 us with interrupts locked (mic.c's method, shorter: the endpoints' reads are
 * 0.1 % of it; the rate only needs to be about right). */
static uint32_t tim7_hz(void)
{
	TIM7->CR1 = 0;
	TIM7->PSC = 7;
	TIM7->ARR = 0xFFFF;
	TIM7->EGR = TIM_EGR_UG;

	unsigned int key = irq_lock();

	TIM7->CR1 = TIM_CR1_CEN;
	uint32_t c0 = DWT->CYCCNT, t0 = TIM7->CNT;

	while (DWT->CYCCNT - c0 < SystemCoreClock / 10000u) {
	}
	uint32_t t1 = TIM7->CNT, c1 = DWT->CYCCNT;

	irq_unlock(key);
	TIM7->CR1 = 0;
	return (uint32_t)((uint64_t)((t1 - t0) & 0xFFFF) * 8u * SystemCoreClock / (c1 - c0));
}

int zprof_port_sample_start(uint32_t hz)
{
	if (hz < 100u || hz > 20000u) {
		return -EINVAL;
	}
	__HAL_RCC_TIM7_CLK_ENABLE();
	__HAL_RCC_TIM7_CLK_SLEEP_ENABLE();   /* the N6's sleep enables reset to 0 (CLAUDE.md) */
	IRQ_DIRECT_CONNECT(TIM7_IRQn, 0, zprof_sample_irq, 0);

	uint32_t mhz = (tim7_hz() + 500000u) / 1000000u;
	uint32_t period = 1000000u / hz;   /* microseconds */

	TIM7->CR1 = 0;
	TIM7->DIER = 0;
	TIM7->PSC = mhz - 1u;
	arr_base = (uint16_t)(period - 1u);
	arr_jit = (uint16_t)(period / 8u);
	TIM7->ARR = arr_base;
	TIM7->EGR = TIM_EGR_UG;            /* PSC and ARR now */
	TIM7->SR = 0;
	NVIC_ClearPendingIRQ(TIM7_IRQn);
	TIM7->DIER = TIM_DIER_UIE;
	irq_enable(TIM7_IRQn);
	TIM7->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;   /* ARR buffered: a new period from the next one */
	return (int)(1000000u / period);
}

void zprof_port_sample_stop(void)
{
	irq_disable(TIM7_IRQn);
	TIM7->CR1 = 0;
	TIM7->DIER = 0;
	TIM7->SR = 0;
	NVIC_ClearPendingIRQ(TIM7_IRQn);
}

/* From zprof's handler (in RAM like it: no flash fetch on a cold cache). Two posted
 * writes and no read: zprof's record after it takes longer than the clear needs to
 * reach the timer (a late clear would show as samples above the rate asked). */
__ramfunc void zprof_port_sample_ack(void)
{
	TIM7->SR = 0;
	lfsr = (lfsr >> 1) ^ ((0u - (lfsr & 1u)) & 0xB400u);
	TIM7->ARR = arr_base - arr_jit + lfsr % (2u * arr_jit + 1u);
}
#endif /* CONFIG_ZPROF_SAMPLE */

#endif /* CONFIG_ZPROF */
