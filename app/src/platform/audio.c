/*
 * Audio output. See audio.h.
 *
 * SAI1 block A is the I2S master; a single circular DMA transfer runs over a
 * two-half buffer and never stops. The DMA's half-transfer and
 * transfer-complete interrupts wake a thread that refills the half that has
 * just been played. This is the arrangement ST's own BSP uses.
 *
 * Why not Zephyr's I2S driver: it starts a new DMA transfer from the
 * interrupt at the end of every block. The SAI FIFO holds 8 samples (83 us at
 * 48 kHz stereo), so any interrupt latency longer than that at a block
 * boundary underruns the SAI, which then stays out of step with its DMA. That
 * was audible as scratchy sound and measurable as blocks draining four times
 * too fast.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <soc.h>
#include <stm32n6xx_hal.h>
#include <stm32n6xx_ll_rcc.h>

#include <zephyr/cache.h>

#include "audio.h"
#include "gfx.h"
#include "psram.h"

LOG_MODULE_REGISTER(audio, LOG_LEVEL_INF);

#define HALF_FRAMES AUDIO_BLOCK_FRAMES          /* 512 frames: 10.7 ms */
#define HALF_WORDS  (HALF_FRAMES * 2)           /* 16-bit samples per half */
#define MCLK_HZ     12288000                    /* 256 x 48 kHz, IC7 = PLL2 / 125 */

#define AUDIO_NODE DT_NODELABEL(audio_out)
#define CODEC_NODE DT_NODELABEL(wm8904)

static bool ok;
static audio_source_t source;
static void *source_ctx;
static struct k_spinlock lock;
static volatile uint32_t mark_us;        /* when the DMA last finished a half */
static volatile uint32_t n_halves, n_errors;
static uint16_t peak_l, peak_r;
/* test tone: overrides the source while tone_left > 0 */
static volatile uint32_t tone_left, tone_step, tone_phase;
/* capture of what is handed to the DAC, for tests (tools/acap.py) */
#define CAP_FRAMES (3 * AUDIO_RATE)
static int16_t *cap_buf;
static volatile uint32_t cap_fill, cap_left;

#if DT_NODE_HAS_STATUS_OKAY(AUDIO_NODE) && DT_NODE_HAS_STATUS_OKAY(CODEC_NODE)

PINCTRL_DT_DEFINE(AUDIO_NODE);
static const struct device *const codec = DEVICE_DT_GET(CODEC_NODE);

static SAI_HandleTypeDef hsai;
static DMA_HandleTypeDef hdma;
static DMA_QListTypeDef dma_queue;
/* The DMA engine reads its linked-list node and the samples straight from
 * memory, and where that memory is matters more than anything else here.
 *
 * It is AHBSRAM1: 16 KB on the AHB bus, beside the DMA controller. Every
 * other RAM in the chip (FLEXRAM, AXISRAM1..6) sits behind the AXI
 * interconnect, and when the CPU streams writes into AXISRAM (LVGL filling a
 * large part of the frame buffer) the DMA's small reads on that interconnect
 * are held off for longer than the SAI FIFO lasts. Measured: about 60
 * underruns a second on screens with big redraws, none on idle screens, with
 * the buffer in AXISRAM1; reads and PSRAM traffic did not matter.
 *
 * The area is ordinary cached memory to the CPU, so every CPU write is
 * followed by a cache flush.
 */
#define RING_WORDS (2 * HALF_WORDS)
#define ring       ((int16_t *)SRAM1_AHB_BASE_S)
#define dma_node   ((DMA_NodeTypeDef *)(SRAM1_AHB_BASE_S + 0x1000))
BUILD_ASSERT(RING_WORDS * sizeof(int16_t) <= 0x1000);

static K_SEM_DEFINE(refill, 0, 2);
static volatile uint8_t refill_half;     /* half the DMA has just finished playing */

/* ---- interrupts ---------------------------------------------------------- */

static void dma_isr(const void *arg)
{
	ARG_UNUSED(arg);
	HAL_DMA_IRQHandler(&hdma);
}

static void sai_isr(const void *arg)
{
	ARG_UNUSED(arg);
	HAL_SAI_IRQHandler(&hsai);
}

static volatile uint32_t t_first_us, t_last_us, restart_ret;

static void half_done(uint8_t half)
{
	refill_half = half;
	mark_us = gfx_now_us();
	if (n_halves == 0) {
		t_first_us = mark_us;
	}
	t_last_us = mark_us;
	n_halves++;
	k_sem_give(&refill);
}

void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *h)
{
	ARG_UNUSED(h);
	half_done(0);
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *h)
{
	ARG_UNUSED(h);
	half_done(1);
}

static volatile uint32_t last_sai_err, last_dma_err, last_dma_csr, first_err_ms;
static volatile bool need_restart;
static volatile uint32_t n_restarts;

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *h)
{
	if (n_errors == 0) {
		first_err_ms = k_uptime_get_32();
	}
	n_errors++;

	last_sai_err = HAL_SAI_GetError(h);
	last_dma_err = hdma.ErrorCode;
	last_dma_csr = GPDMA1_Channel2->CSR;
	/* After an underrun the SAI and its DMA stay out of step (heard as a steady
	 * tick). Carrying on is not an option: have the thread restart the stream.
	 */
	need_restart = true;
	k_sem_give(&refill);
}

/* Start the circular transfer. HAL_SAI_Transmit_DMA() writes the buffer
 * address and length into the linked-list node and enables the channel in one
 * go, and the DMA engine then loads the node from memory. Writing the same
 * values first and flushing the cache guarantees it loads what the HAL meant.
 */
static HAL_StatusTypeDef stream_start(void)
{
	memset(ring, 0, RING_WORDS * sizeof(int16_t));
	sys_cache_data_flush_range(ring, RING_WORDS * sizeof(int16_t));
	dma_node->LinkRegisters[NODE_CBR1_DEFAULT_OFFSET] = RING_WORDS * sizeof(int16_t);
	dma_node->LinkRegisters[NODE_CSAR_DEFAULT_OFFSET] = (uint32_t)ring;
	dma_node->LinkRegisters[NODE_CDAR_DEFAULT_OFFSET] = (uint32_t)&SAI1_Block_A->DR;
	sys_cache_data_flush_range(dma_node, sizeof(*dma_node));
	mark_us = gfx_now_us();
	return HAL_SAI_Transmit_DMA(&hsai, (uint8_t *)ring, RING_WORDS);
}

/* Stop and restart the stream, clearing the counters. */
void audio_restart(void)
{
	HAL_SAI_DMAStop(&hsai);
	n_halves = 0;
	n_errors = 0;
	restart_ret = stream_start();
}

/* Instantaneous frame rate from the DMA byte counter, eight 4 ms windows.
 * Independent of the half/full interrupts, so it also shows a wandering clock.
 */
int audio_rate_probe(char *buf, size_t len)
{
	int n = snprintf(buf, len, "frame rate, Hz:");

	for (int i = 0; i < 8; i++) {
		uint32_t p0 = GPDMA1_Channel2->CBR1 & 0xffff, t0 = gfx_now_us();

		k_busy_wait(4000);
		uint32_t p1 = GPDMA1_Channel2->CBR1 & 0xffff, t1 = gfx_now_us();
		uint32_t bytes = (p0 - p1) & (RING_WORDS * 2 - 1);

		n += snprintf(buf + n, len - n, " %u", (unsigned int)((uint64_t)bytes * 250000 / (t1 - t0)));
		k_msleep(20);
	}
	n += snprintf(buf + n, len - n, "\n");
	return n;
}

int audio_debug(char *buf, size_t len)
{
	return snprintf(buf, len,
			"restarts %u underruns %u "
			"halves=%u in %u us (%u Hz frame rate) restart_ret=%u hal_err=%x "
			"sai_err=%x dma_err=%x dma_csr_at_err=%x first_err_ms=%u sai_state=%u SR=%x CR1=%x "
			"dma CSR=%x CCR=%x CBR1=%x CLLR=%x CTR1=%x CTR2=%x CSAR=%x CDAR=%x ring=%p node=%p\n",
			(unsigned int)n_restarts, (unsigned int)n_errors,
			(unsigned int)n_halves, (unsigned int)(t_last_us - t_first_us),
			n_halves > 1 ? (unsigned int)((uint64_t)(n_halves - 1) * HALF_FRAMES * 1000000 / (t_last_us - t_first_us)) : 0,
			(unsigned int)restart_ret, (unsigned int)HAL_SAI_GetError(&hsai),
			(unsigned int)last_sai_err, (unsigned int)last_dma_err, (unsigned int)last_dma_csr,
			(unsigned int)first_err_ms, (unsigned int)hsai.State, (unsigned int)SAI1_Block_A->SR,
			(unsigned int)SAI1_Block_A->CR1, (unsigned int)GPDMA1_Channel2->CSR,
			(unsigned int)GPDMA1_Channel2->CCR, (unsigned int)GPDMA1_Channel2->CBR1,
			(unsigned int)GPDMA1_Channel2->CLLR, (unsigned int)GPDMA1_Channel2->CTR1,
			(unsigned int)GPDMA1_Channel2->CTR2, (unsigned int)GPDMA1_Channel2->CSAR,
			(unsigned int)GPDMA1_Channel2->CDAR, (void *)ring, (void *)dma_node);
}

static HAL_StatusTypeDef stream_start(void);

/* ---- refill thread ------------------------------------------------------- */

static void audio_thread(void *p1, void *p2, void *p3)
{
	for (;;) {
		k_sem_take(&refill, K_FOREVER);
		if (need_restart) {
			need_restart = false;
			HAL_SAI_DMAStop(&hsai);
			k_sem_reset(&refill);
			stream_start();
			n_restarts++;
			continue;
		}

		int16_t *dst = ring + refill_half * HALF_WORDS;
		uint32_t got = 0;
		int l = 0, r = 0;
		k_spinlock_key_t key = k_spin_lock(&lock);
		audio_source_t fn = source;
		void *ctx = source_ctx;

		k_spin_unlock(&lock, key);
		if (tone_left > 0) {
			/* 32-bit phase accumulator through a parabolic sine: clean enough to hear faults by ear */
			for (got = 0; got < HALF_FRAMES && tone_left > 0; got++, tone_left--) {
				int32_t x = (int32_t)(tone_phase >> 16) - 32768;          /* -32768..32767 saw */
				int32_t y = (x * (32768 - abs(x))) >> 13;                  /* parabola, peak 32768 */
				int16_t v = (int16_t)CLAMP(y * 8192 / 32768, -8192, 8192); /* -12 dBFS */

				dst[2 * got] = dst[2 * got + 1] = v;
				tone_phase += tone_step;
			}
		} else if (fn) {
			got = MIN(fn(dst, HALF_FRAMES, ctx), (uint32_t)HALF_FRAMES);
		}
		memset(dst + got * 2, 0, (HALF_FRAMES - got) * 4);
		if (cap_left > 0) {
			uint32_t n = MIN(cap_left, (uint32_t)HALF_FRAMES);

			memcpy(cap_buf + cap_fill * 2, dst, n * 4);
			cap_fill += n;
			cap_left -= n;
		}
		for (uint32_t i = 0; i < got; i += 4) { /* a coarse peak is enough for a meter */
			l = MAX(l, abs(dst[2 * i]));
			r = MAX(r, abs(dst[2 * i + 1]));
		}
		peak_l = l;
		peak_r = r;
		sys_cache_data_flush_range(dst, HALF_WORDS * sizeof(int16_t));
	}
}
/* Cooperative and above the UI: a refill is a 2 KB copy and must not be late. */
K_THREAD_DEFINE(audio_tid, 2048, audio_thread, NULL, NULL, NULL, K_PRIO_COOP(2), 0, 0);

/* ---- setup --------------------------------------------------------------- */

static int dma_setup(void)
{
	DMA_NodeConfTypeDef nc = {0};

	/* Run AND sleep: on the N6 a peripheral clock enabled only in RCC_xxxENR
	 * stops whenever the CPU executes WFI, which the idle thread does many
	 * times a frame. Zephyr's clock driver sets both bits; the HAL macro for
	 * "enable" sets only the first. With the sleep bit clear the DMA and the
	 * SAI froze during every idle period: underruns, then a stalled stream.
	 */
	__HAL_RCC_GPDMA1_CLK_ENABLE();
	__HAL_RCC_GPDMA1_CLK_SLEEP_ENABLE();
	__HAL_RCC_AHBSRAM1_MEM_CLK_ENABLE();
	__HAL_RCC_AHBSRAM1_MEM_CLK_SLEEP_ENABLE();
	hdma.Instance = GPDMA1_Channel2;

	/* prove the RAM is really there before trusting the DMA to it */
	ring[0] = 0x5aa5;
	ring[RING_WORDS - 1] = 0x1234;
	sys_cache_data_flush_and_invd_range(ring, RING_WORDS * sizeof(int16_t));
	if (ring[0] != 0x5aa5 || ring[RING_WORDS - 1] != 0x1234) {
		LOG_ERR("AHBSRAM1 not readable");
		return -EIO;
	}

	nc.NodeType = DMA_GPDMA_LINEAR_NODE;
	nc.Init.Request = GPDMA1_REQUEST_SAI1_A;
	nc.Init.BlkHWRequest = DMA_BREQ_SINGLE_BURST;
	nc.Init.Direction = DMA_MEMORY_TO_PERIPH;
	nc.Init.SrcInc = DMA_SINC_INCREMENTED;
	nc.Init.DestInc = DMA_DINC_FIXED;
	nc.Init.SrcDataWidth = DMA_SRC_DATAWIDTH_HALFWORD;
	nc.Init.DestDataWidth = DMA_DEST_DATAWIDTH_HALFWORD;
	nc.Init.SrcBurstLength = 1;
	nc.Init.DestBurstLength = 1;
	nc.Init.Priority = DMA_HIGH_PRIORITY;
	nc.Init.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
	/* As in ST's BSP: memory side on port 0, peripheral side and link loads on port 1. */
	nc.Init.TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
	nc.DataHandlingConfig.DataExchange = DMA_EXCHANGE_NONE;
	nc.DataHandlingConfig.DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED;
	nc.TriggerConfig.TriggerPolarity = DMA_TRIG_POLARITY_MASKED;
	nc.SrcSecure = DMA_CHANNEL_SRC_SEC;
	nc.DestSecure = DMA_CHANNEL_DEST_SEC;

	memset(dma_node, 0, sizeof(*dma_node));
	if (HAL_DMAEx_List_BuildNode(&nc, dma_node) != HAL_OK ||
	    HAL_DMAEx_List_InsertNode_Tail(&dma_queue, dma_node) != HAL_OK ||
	    HAL_DMAEx_List_SetCircularMode(&dma_queue) != HAL_OK) {
		return -EIO;
	}
	hdma.InitLinkedList.Priority = DMA_HIGH_PRIORITY;
	hdma.InitLinkedList.LinkStepMode = DMA_LSM_FULL_EXECUTION;
	hdma.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT1;
	hdma.InitLinkedList.TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER;
	hdma.InitLinkedList.LinkedListMode = DMA_LINKEDLIST_CIRCULAR;
	if (HAL_DMAEx_List_Init(&hdma) != HAL_OK || HAL_DMAEx_List_LinkQ(&hdma, &dma_queue) != HAL_OK ||
	    HAL_DMA_ConfigChannelAttributes(&hdma, DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC | DMA_CHANNEL_SRC_SEC |
							   DMA_CHANNEL_DEST_SEC) != HAL_OK) {
		return -EIO;
	}
	__HAL_LINKDMA(&hsai, hdmatx, hdma);

	IRQ_CONNECT(GPDMA1_Channel2_IRQn, 1, dma_isr, NULL, 0);
	irq_enable(GPDMA1_Channel2_IRQn);
	IRQ_CONNECT(SAI1_A_IRQn, 1, sai_isr, NULL, 0);
	irq_enable(SAI1_A_IRQn);
	return 0;
}

static int sai_setup(void)
{
	/* Kernel clock: IC7, enabled and divided by the Zephyr clock driver (see the overlay). */
	LL_RCC_SetSAIClockSource(LL_RCC_SAI1_CLKSOURCE_IC7);
	__HAL_RCC_SAI1_CLK_ENABLE();
	__HAL_RCC_SAI1_CLK_SLEEP_ENABLE(); /* see dma_setup() */
	__HAL_RCC_SAI1_FORCE_RESET();
	__HAL_RCC_SAI1_RELEASE_RESET();

	hsai.Instance = SAI1_Block_A;
	hsai.Init.AudioMode = SAI_MODEMASTER_TX;
	hsai.Init.Synchro = SAI_ASYNCHRONOUS;
	hsai.Init.SynchroExt = SAI_SYNCEXT_DISABLE;
	hsai.Init.OutputDrive = SAI_OUTPUTDRIVE_ENABLE;
	hsai.Init.NoDivider = SAI_MASTERDIVIDER_ENABLE;
	/* Three quarters: the DMA is asked for data while fewer than 6 of the 8
	 * FIFO slots are filled. That leaves about 60 us of audio in the FIFO if
	 * the DMA is held up, and two free slots for the sample the DMA may still
	 * write after the request line drops. Measured alternatives: "full"
	 * over-delivers (samples lost, playback runs fast), "quarter" leaves only
	 * 20 us and underruns.
	 */
	hsai.Init.FIFOThreshold = SAI_FIFOTHRESHOLD_3QF;
	hsai.Init.AudioFrequency = SAI_AUDIO_FREQUENCY_48K;
	hsai.Init.MckOutput = SAI_MCK_OUTPUT_ENABLE;
	hsai.Init.MckOverSampling = SAI_MCK_OVERSAMPLING_DISABLE;
	hsai.Init.MonoStereoMode = SAI_STEREOMODE;
	hsai.Init.CompandingMode = SAI_NOCOMPANDING;
	hsai.Init.TriState = SAI_OUTPUT_NOTRELEASED;
	hsai.Init.Protocol = SAI_FREE_PROTOCOL;
	hsai.Init.DataSize = SAI_DATASIZE_16;
	hsai.Init.FirstBit = SAI_FIRSTBIT_MSB;
	hsai.Init.ClockStrobing = SAI_CLOCKSTROBING_FALLINGEDGE;
	/* I2S framing: 32 bit clocks per frame, FS low for the left channel, data one bit after the FS edge */
	hsai.FrameInit.FrameLength = 32;
	hsai.FrameInit.ActiveFrameLength = 16;
	hsai.FrameInit.FSDefinition = SAI_FS_CHANNEL_IDENTIFICATION;
	hsai.FrameInit.FSPolarity = SAI_FS_ACTIVE_LOW;
	hsai.FrameInit.FSOffset = SAI_FS_BEFOREFIRSTBIT;
	hsai.SlotInit.FirstBitOffset = 0;
	hsai.SlotInit.SlotSize = SAI_SLOTSIZE_16B;
	hsai.SlotInit.SlotNumber = 2;
	hsai.SlotInit.SlotActive = SAI_SLOTACTIVE_0 | SAI_SLOTACTIVE_1;

	return HAL_SAI_Init(&hsai) == HAL_OK ? 0 : -EIO;
}

int audio_init(void)
{
	struct audio_codec_cfg ccfg = {
		.mclk_freq = MCLK_HZ,
		.dai_type = AUDIO_DAI_TYPE_I2S,
		.dai_route = AUDIO_ROUTE_PLAYBACK,
		.dai_cfg.i2s = {
			.word_size = 16,
			.channels = 2,
			.format = I2S_FMT_DATA_FORMAT_I2S,
			/* from the codec's side: it is the clock target */
			.options = I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET,
			.frame_clk_freq = AUDIO_RATE,
		},
	};
	int ret;

	if (!device_is_ready(codec)) {
		LOG_ERR("codec not ready");
		return -ENODEV;
	}

	ret = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(AUDIO_NODE), PINCTRL_STATE_DEFAULT);
	if (ret == 0) {
		ret = dma_setup();
	}
	if (ret == 0) {
		ret = sai_setup(); /* MCLK runs from here on; the codec needs it for its own setup */
	}
	if (ret != 0) {
		LOG_ERR("SAI/DMA setup failed (%d)", ret);
		return ret;
	}
	k_msleep(10);
	ret = audio_codec_configure(codec, &ccfg);
	if (ret != 0) {
		LOG_ERR("codec configure failed (%d)", ret);
		return ret;
	}
	audio_codec_start_output(codec);

	if (stream_start() != HAL_OK) {
		LOG_ERR("HAL_SAI_Transmit_DMA failed, error 0x%x", (unsigned int)HAL_SAI_GetError(&hsai));
		return -EIO;
	}
	ok = true;
	audio_set_volume(60);
	LOG_INF("audio out: 48 kHz stereo, circular DMA over 2 x %d frames, SAI clock %u Hz", HALF_FRAMES,
		(unsigned int)HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SAI1));
	return 0;
}

void audio_set_volume(int percent)
{
	/* WM8904 headphone PGA: 0..63 in 1 dB steps, 57 is 0 dB. Stay at or below 0 dB. */
	audio_property_value_t v = {.vol = CLAMP(percent, 0, 100) * 57 / 100};

	if (ok) {
		audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, v);
		audio_codec_apply_properties(codec);
	}
}

#else /* no audio node */

/* Start the circular transfer. HAL_SAI_Transmit_DMA() writes the buffer
 * address and length into the linked-list node and enables the channel in one
 * go, and the DMA engine then loads the node from memory. Writing the same
 * values first and flushing the cache guarantees it loads what the HAL meant.
 */
static HAL_StatusTypeDef stream_start(void)
{
	memset(ring, 0, RING_WORDS * sizeof(int16_t));
	sys_cache_data_flush_range(ring, RING_WORDS * sizeof(int16_t));
	dma_node->LinkRegisters[NODE_CBR1_DEFAULT_OFFSET] = RING_WORDS * sizeof(int16_t);
	dma_node->LinkRegisters[NODE_CSAR_DEFAULT_OFFSET] = (uint32_t)ring;
	dma_node->LinkRegisters[NODE_CDAR_DEFAULT_OFFSET] = (uint32_t)&SAI1_Block_A->DR;
	sys_cache_data_flush_range(dma_node, sizeof(*dma_node));
	mark_us = gfx_now_us();
	return HAL_SAI_Transmit_DMA(&hsai, (uint8_t *)ring, RING_WORDS);
}

/* Stop and restart the stream, clearing the counters. */
void audio_restart(void)
{
	HAL_SAI_DMAStop(&hsai);
	n_halves = 0;
	n_errors = 0;
	restart_ret = stream_start();
}

/* Instantaneous frame rate from the DMA byte counter, eight 4 ms windows.
 * Independent of the half/full interrupts, so it also shows a wandering clock.
 */
int audio_rate_probe(char *buf, size_t len)
{
	int n = snprintf(buf, len, "frame rate, Hz:");

	for (int i = 0; i < 8; i++) {
		uint32_t p0 = GPDMA1_Channel2->CBR1 & 0xffff, t0 = gfx_now_us();

		k_busy_wait(4000);
		uint32_t p1 = GPDMA1_Channel2->CBR1 & 0xffff, t1 = gfx_now_us();
		uint32_t bytes = (p0 - p1) & (RING_WORDS * 2 - 1);

		n += snprintf(buf + n, len - n, " %u", (unsigned int)((uint64_t)bytes * 250000 / (t1 - t0)));
		k_msleep(20);
	}
	n += snprintf(buf + n, len - n, "\n");
	return n;
}

int audio_debug(char *buf, size_t len)
{
	return snprintf(buf, len, "no audio\n");
}

void audio_restart(void)
{
}

int audio_rate_probe(char *buf, size_t len)
{
	return snprintf(buf, len, "no audio\n");
}

int audio_init(void)
{
	return -ENODEV;
}

void audio_set_volume(int percent)
{
	ARG_UNUSED(percent);
}

#endif

bool audio_ok(void)
{
	return ok;
}

void audio_set_source(audio_source_t fn, void *ctx)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	source = fn;
	source_ctx = ctx;
	k_spin_unlock(&lock, key);
}

uint32_t audio_queued_frames(void)
{
	if (!ok) {
		return 0;
	}
	/* When a half finishes it is refilled at once; that fresh half starts
	 * playing after the other one, so two halves are queued at that instant
	 * and the count runs down at the sample rate until the next event.
	 */
	uint32_t drained = (uint32_t)((uint64_t)(gfx_now_us() - mark_us) * AUDIO_RATE / 1000000);

	return 2 * HALF_FRAMES - MIN(drained, (uint32_t)HALF_FRAMES);
}

void audio_tone(uint32_t hz, uint32_t ms)
{
	tone_step = (uint32_t)(((uint64_t)hz << 32) / AUDIO_RATE);
	tone_phase = 0;
	tone_left = ms * (AUDIO_RATE / 1000);
}

void audio_get_counters(uint32_t *blocks, uint32_t *errors)
{
	*blocks = n_halves;
	*errors = n_errors;
}

void audio_get_peak(uint16_t *left, uint16_t *right)
{
	*left = peak_l;
	*right = peak_r;
}

int audio_capture(uint32_t ms, char *buf, size_t len)
{
	if (ms > 0) {
		if (cap_buf == NULL) {
			cap_buf = psram_alloc(CAP_FRAMES * 4, 32);
		}
		if (cap_buf == NULL) {
			return snprintf(buf, len, "err no memory\n");
		}
		cap_left = 0; /* the refill thread is above this one: stop it first */
		cap_fill = 0;
		cap_left = MIN(ms * (AUDIO_RATE / 1000), (uint32_t)CAP_FRAMES);
	}
	return snprintf(buf, len, "addr=0x%08x frames=%u filled=%u\n", (unsigned int)(uintptr_t)cap_buf,
			(unsigned int)(cap_fill + cap_left), (unsigned int)cap_fill);
}
