/*
 * MEMS microphone through MDF1. See mic.h.
 *
 * U13 (MP23DB01) gets its clock from MDF1_CCK0 on PE2 and answers with a
 * 1-bit stream on MDF1_SDI0, PE8. Filter 0 decimates it:
 *
 *   kernel clock  IC7 = 12.288 MHz (the SAI's MCLK source: same PLL, so the
 *                 microphone and the DAC share one sample clock)
 *   CCK0          12.288 MHz / 4 = 3.072 MHz
 *   Sinc5, / 16   192 kHz
 *   reshape, / 4  48 kHz, pass band to 21 kHz
 *   high pass     0.000625 x 48 kHz = 30 Hz (a PDM stream has a DC offset)
 *
 * which is the 48 kHz recipe of RM0486 table 290 (row 11) and AN5795. The
 * serial interface, bit-stream edge, pins and DMA request are those of ST's
 * MDF_AudioRecorder example, which records this microphone on this board.
 *
 * The filter's FIFO holds four samples (83 us at 48 kHz), the same margin as
 * the SAI's on the way out, so the DMA buffer and its list node are in
 * AHBSRAM1 next to the audio output's, for the reason given in audio.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <soc.h>
#include <stm32n6xx_hal.h>
#include <stm32n6xx_ll_gpio.h>
#include <stm32n6xx_ll_rcc.h>

#include "gfx.h"
#include "mic.h"
#include "psram.h"

LOG_MODULE_REGISTER(mic, LOG_LEVEL_INF);

#define HALF      MIC_BLOCK /* samples per DMA half */
#define DMA_WORDS (2 * HALF)
/* AHBSRAM1: the audio output has 0x0000 (buffer) and 0x1000 (node) */
#define dma_buf   ((int32_t *)(SRAM1_AHB_BASE_S + 0x2000))
#define dma_node  ((DMA_NodeTypeDef *)(SRAM1_AHB_BASE_S + 0x3000))
BUILD_ASSERT(DMA_WORDS * sizeof(int32_t) <= 0x1000);
BUILD_ASSERT((MIC_KEEP & (MIC_KEEP - 1)) == 0);

#define CAP_SAMPLES (3 * MIC_RATE)

static MDF_HandleTypeDef hmdf;
static DMA_HandleTypeDef hdma;
static DMA_QListTypeDef dma_queue;
static MDF_FilterConfigTypeDef filter;

static bool ok;
static int32_t *ring; /* MIC_KEEP samples in PSRAM */
static volatile uint32_t total;
static volatile uint32_t n_blocks, n_errors, last_error;
static volatile uint32_t rate_hz;
static volatile int32_t blk_peak, blk_rms, blk_dc;
static volatile uint8_t ready_half;
static K_SEM_DEFINE(block, 0, 2);

static int32_t *cap_buf;
static volatile uint32_t cap_fill, cap_left;

/* ---- interrupts ---------------------------------------------------------- */

static void mic_dma_isr(const void *arg)
{
	ARG_UNUSED(arg);
	HAL_DMA_IRQHandler(&hdma);
}

static void mdf_isr(const void *arg)
{
	ARG_UNUSED(arg);
	HAL_MDF_IRQHandler(&hmdf);
}

void HAL_MDF_AcqHalfCpltCallback(MDF_HandleTypeDef *h)
{
	ARG_UNUSED(h);
	ready_half = 0;
	k_sem_give(&block);
}

void HAL_MDF_AcqCpltCallback(MDF_HandleTypeDef *h)
{
	ARG_UNUSED(h);
	ready_half = 1;
	k_sem_give(&block);
}

void HAL_MDF_ErrorCallback(MDF_HandleTypeDef *h)
{
	last_error = HAL_MDF_GetError(h);
	n_errors++;
}

/* ---- the thread ---------------------------------------------------------- */

static void mic_thread(void *p1, void *p2, void *p3)
{
	uint32_t t_mark = 0, n_mark = 0;

	for (;;) {
		k_sem_take(&block, K_FOREVER);

		const int32_t *src = dma_buf + ready_half * HALF;
		uint32_t at = total;
		int64_t sum = 0, sq = 0;
		int32_t peak = 0;

		/* the DMA wrote this behind the cache's back */
		sys_cache_data_invd_range((void *)src, HALF * sizeof(int32_t));
		for (int i = 0; i < HALF; i++) {
			int32_t s = src[i] >> 8; /* 24 bits, left-aligned in the data register */

			ring[(at + i) & (MIC_KEEP - 1)] = s;
			sum += s;
			sq += (int64_t)s * s;
			peak = MAX(peak, abs(s));
		}
		if (cap_left > 0) {
			uint32_t n = MIN(cap_left, (uint32_t)HALF);

			for (uint32_t i = 0; i < n; i++) {
				cap_buf[cap_fill + i] = ring[(at + i) & (MIC_KEEP - 1)];
			}
			cap_fill += n;
			cap_left -= n;
		}
		total = at + HALF;
		blk_peak = peak;
		blk_dc = (int32_t)(sum / HALF);
		blk_rms = (int32_t)sqrtf((float)(sq / HALF));

		uint32_t now = gfx_now_us();

		if (++n_blocks == 1) {
			t_mark = now;
			n_mark = 1;
		} else if (n_blocks - n_mark >= MIC_RATE / HALF) { /* about a second */
			rate_hz = (uint32_t)((uint64_t)(n_blocks - n_mark) * HALF * 1000000 / (now - t_mark));
			t_mark = now;
			n_mark = n_blocks;
		}
	}
}
/* Cooperative, just below the audio output's refill: a 512-byte copy every
 * 2.7 ms that has to happen before the DMA comes round again.
 */
K_THREAD_DEFINE(mic_tid, 2048, mic_thread, NULL, NULL, NULL, K_PRIO_COOP(3), 0, 0);

/* ---- setup --------------------------------------------------------------- */

static int dma_setup(void)
{
	DMA_NodeConfTypeDef nc = {0};

	/* run and sleep enables: see audio.c */
	__HAL_RCC_GPDMA1_CLK_ENABLE();
	__HAL_RCC_GPDMA1_CLK_SLEEP_ENABLE();
	__HAL_RCC_AHBSRAM1_MEM_CLK_ENABLE();
	__HAL_RCC_AHBSRAM1_MEM_CLK_SLEEP_ENABLE();
	hdma.Instance = GPDMA1_Channel6;

	nc.NodeType = DMA_GPDMA_LINEAR_NODE;
	nc.Init.Request = GPDMA1_REQUEST_MDF1_FLT0;
	nc.Init.BlkHWRequest = DMA_BREQ_SINGLE_BURST;
	nc.Init.Direction = DMA_PERIPH_TO_MEMORY;
	nc.Init.SrcInc = DMA_SINC_FIXED;
	nc.Init.DestInc = DMA_DINC_INCREMENTED;
	nc.Init.SrcDataWidth = DMA_SRC_DATAWIDTH_WORD;
	nc.Init.DestDataWidth = DMA_DEST_DATAWIDTH_WORD;
	nc.Init.SrcBurstLength = 1;
	nc.Init.DestBurstLength = 1;
	nc.Init.Priority = DMA_HIGH_PRIORITY;
	nc.Init.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
	/* as for the audio output: the memory side on port 0, the peripheral and the link loads on port 1 */
	nc.Init.TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT1 | DMA_DEST_ALLOCATED_PORT0;
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
	__HAL_LINKDMA(&hmdf, hdma, hdma);

	IRQ_CONNECT(GPDMA1_Channel6_IRQn, 1, mic_dma_isr, NULL, 0);
	irq_enable(GPDMA1_Channel6_IRQn);
	IRQ_CONNECT(MDF1_FLT0_IRQn, 1, mdf_isr, NULL, 0);
	irq_enable(MDF1_FLT0_IRQn);
	return 0;
}

/* The settings that decide whether the bit stream is read properly; the rest
 * is fixed. mic_tune() restarts the capture with other values (bring-up).
 *
 * Low-frequency master mode: the MDF samples the data line with its own
 * processing clock. In normal SPI mode the processing clock would be exactly
 * 4 times the microphone clock here (12.288 / 3.072 MHz), and RM0486 asks for
 * more than 4. Both recorded the same on a working board (2026-11-17, quiet
 * -96 dBFS, claps 41 to 63 dB above it); this one is within the rules.
 */
static struct {
	int lf_master; /* serial interface in low-frequency master mode, not "normal SPI" */
	int falling;   /* take the bits on the falling clock edge */
	int proc_div;  /* kernel clock / this = processing clock */
	int cck_div;   /* processing clock / this = microphone clock */
	int sinc;      /* 4 or 5 */
	int dec;       /* CIC decimation; x4 in the reshape filter */
	int gain;      /* about 3 dB a step */
	int clk;       /* kernel clock: 0 IC7, 1 HCLK, 2 HSI, 3 CLKP, 4 IC8, 5 MSI */
	int pull;      /* on the data pin: 0 none, 1 up, 2 down (is anything driving it?) */
	int speed;     /* drive strength of the clock pin, 0 (slow edges) to 3 */
	int hpf;       /* high-pass filter: 1 on, 0 bypassed (to see the DC level) */
	int rsf;       /* reshape filter: 1 on (/4), 0 bypassed */
} cfg = {.lf_master = 1, .falling = 1, .proc_div = 1, .cck_div = 4, .sinc = 5, .dec = 16, .gain = 1, .speed = 3, .hpf = 1, .rsf = 1};

static int start(void)
{
	MDF_DmaConfigTypeDef dma = {
		.Address = (uint32_t)dma_buf,
		.DataLength = DMA_WORDS * sizeof(int32_t),
		.MsbOnly = DISABLE,
	};

	static const uint32_t clk[] = {LL_RCC_MDF1_CLKSOURCE_IC7, LL_RCC_MDF1_CLKSOURCE_HCLK, LL_RCC_MDF1_CLKSOURCE_HSI,
				       LL_RCC_MDF1_CLKSOURCE_CLKP, LL_RCC_MDF1_CLKSOURCE_IC8, LL_RCC_MDF1_CLKSOURCE_MSI};

	LL_RCC_SetMDFClockSource(clk[CLAMP(cfg.clk, 0, (int)ARRAY_SIZE(clk) - 1)]);
	LL_GPIO_SetPinSpeed(GPIOE, LL_GPIO_PIN_2, CLAMP(cfg.speed, 0, 3));
	LL_GPIO_SetPinPull(GPIOE, LL_GPIO_PIN_8,
			   cfg.pull == 1 ? LL_GPIO_PULL_UP : cfg.pull == 2 ? LL_GPIO_PULL_DOWN : LL_GPIO_PULL_NO);
	hmdf.Instance = MDF1_Filter0;
	hmdf.Init.CommonParam.InterleavedFilters = 0;
	hmdf.Init.CommonParam.ProcClockDivider = cfg.proc_div;
	hmdf.Init.CommonParam.OutputClock.Activation = ENABLE;
	hmdf.Init.CommonParam.OutputClock.Pins = MDF_OUTPUT_CLOCK_0;
	hmdf.Init.CommonParam.OutputClock.Divider = cfg.cck_div;
	hmdf.Init.CommonParam.OutputClock.Trigger.Activation = DISABLE;
	hmdf.Init.SerialInterface.Activation = ENABLE;
	hmdf.Init.SerialInterface.Mode = cfg.lf_master ? MDF_SITF_LF_MASTER_SPI_MODE : MDF_SITF_NORMAL_SPI_MODE;
	hmdf.Init.SerialInterface.ClockSource = MDF_SITF_CCK0_SOURCE;
	hmdf.Init.SerialInterface.Threshold = 31;
	hmdf.Init.FilterBistream = cfg.falling ? MDF_BITSTREAM0_FALLING : MDF_BITSTREAM0_RISING;
	if (HAL_MDF_Init(&hmdf) != HAL_OK) {
		LOG_ERR("HAL_MDF_Init failed, error 0x%x", (unsigned int)HAL_MDF_GetError(&hmdf));
		return -EIO;
	}

	filter.DataSource = MDF_DATA_SOURCE_BSMX;
	filter.Delay = 0;
	filter.CicMode = cfg.sinc == 4 ? MDF_ONE_FILTER_SINC4 : MDF_ONE_FILTER_SINC5;
	filter.DecimationRatio = cfg.dec;
	filter.Offset = 0;
	filter.Gain = cfg.gain;
	filter.ReshapeFilter.Activation = cfg.rsf ? ENABLE : DISABLE;
	filter.ReshapeFilter.DecimationRatio = MDF_RSF_DECIMATION_RATIO_4;
	filter.HighPassFilter.Activation = cfg.hpf ? ENABLE : DISABLE;
	filter.HighPassFilter.CutOffFrequency = MDF_HPF_CUTOFF_0_000625FPCM;
	filter.Integrator.Activation = DISABLE;
	filter.SoundActivity.Activation = DISABLE;
	filter.AcquisitionMode = MDF_MODE_ASYNC_CONT;
	filter.FifoThreshold = MDF_FIFO_THRESHOLD_NOT_EMPTY;
	filter.DiscardSamples = 32; /* the filters' own settling */

	/* The HAL writes these into the list node and enables the channel at once;
	 * the DMA engine then loads the node from memory. Writing the same values
	 * first and flushing the cache makes sure it loads what the HAL meant.
	 */
	memset(dma_buf, 0, DMA_WORDS * sizeof(int32_t));
	sys_cache_data_flush_range(dma_buf, DMA_WORDS * sizeof(int32_t));
	dma_node->LinkRegisters[NODE_CBR1_DEFAULT_OFFSET] = dma.DataLength;
	dma_node->LinkRegisters[NODE_CSAR_DEFAULT_OFFSET] = (uint32_t)&MDF1_Filter0->DFLTDR;
	dma_node->LinkRegisters[NODE_CDAR_DEFAULT_OFFSET] = dma.Address;
	sys_cache_data_flush_range(dma_node, sizeof(*dma_node));
	if (HAL_MDF_AcqStart_DMA(&hmdf, &filter, &dma) != HAL_OK) {
		LOG_ERR("HAL_MDF_AcqStart_DMA failed, error 0x%x", (unsigned int)HAL_MDF_GetError(&hmdf));
		return -EIO;
	}
	return 0;
}

int mic_init(void)
{
	static const uint32_t pins[] = {LL_GPIO_PIN_2, LL_GPIO_PIN_8};

	ring = psram_alloc(MIC_KEEP * sizeof(int32_t), 32);
	if (ring == NULL) {
		return -ENOMEM;
	}
	memset(ring, 0, MIC_KEEP * sizeof(int32_t));

	/* IC7 is enabled and divided by the Zephyr clock driver for the SAI (see the overlay) */
	LL_RCC_SetMDFClockSource(LL_RCC_MDF1_CLKSOURCE_IC7);
	__HAL_RCC_MDF1_CLK_ENABLE();
	__HAL_RCC_MDF1_CLK_SLEEP_ENABLE();
	__HAL_RCC_MDF1_FORCE_RESET();
	__HAL_RCC_MDF1_RELEASE_RESET();
	__HAL_RCC_GPIOE_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_SLEEP_ENABLE();
	/* PE2 = MDF1_CCK0, PE8 = MDF1_SDI0, both AF4, push-pull, no pull, as in ST's example */
	LL_GPIO_SetAFPin_0_7(GPIOE, LL_GPIO_PIN_2, LL_GPIO_AF_4);
	LL_GPIO_SetAFPin_8_15(GPIOE, LL_GPIO_PIN_8, LL_GPIO_AF_4);
	for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
		LL_GPIO_SetPinSpeed(GPIOE, pins[i], LL_GPIO_SPEED_FREQ_VERY_HIGH);
		LL_GPIO_SetPinOutputType(GPIOE, pins[i], LL_GPIO_OUTPUT_PUSHPULL);
		LL_GPIO_SetPinPull(GPIOE, pins[i], LL_GPIO_PULL_NO);
		LL_GPIO_SetPinMode(GPIOE, pins[i], LL_GPIO_MODE_ALTERNATE);
	}

	if (dma_setup() != 0) {
		LOG_ERR("DMA setup failed");
		return -EIO;
	}
	if (start() != 0) {
		return -EIO;
	}
	ok = true;
	LOG_INF("microphone: MDF1 kernel clock %u Hz, 3.072 MHz PDM, Sinc5 / 16 and reshape / 4: 48 kHz, 24 bit",
		(unsigned int)HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_MDF1));
	return 0;
}

int mic_tune(const int *v, int n, char *buf, size_t len)
{
	int *field[] = {&cfg.lf_master, &cfg.falling, &cfg.proc_div, &cfg.cck_div, &cfg.sinc, &cfg.dec, &cfg.gain, &cfg.clk, &cfg.pull, &cfg.speed, &cfg.hpf,
			&cfg.rsf};

	if (n > 0) {
		ok = false;
		HAL_MDF_AcqStop_DMA(&hmdf);
		HAL_MDF_DeInit(&hmdf);
		for (int i = 0; i < MIN(n, (int)ARRAY_SIZE(field)); i++) {
			*field[i] = v[i];
		}
		n_errors = 0;
		last_error = 0;
		ok = start() == 0;
	}
	return snprintf(buf, len,
			"ok=%d lf_master=%d falling=%d proc_div=%d cck_div=%d sinc=%d dec=%d gain=%d clk=%d pull=%d speed=%d hpf=%d rsf=%d ker_hz=%u "
			"ckgcr=%08x sitfcr=%08x bsmxcr=%08x dfltcr=%08x cicr=%08x rsfr=%08x isr=%08x\n",
			ok, cfg.lf_master, cfg.falling, cfg.proc_div, cfg.cck_div, cfg.sinc, cfg.dec, cfg.gain, cfg.clk, cfg.pull, cfg.speed, cfg.hpf, cfg.rsf,
			(unsigned int)HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_MDF1),
			(unsigned int)MDF1->CKGCR, (unsigned int)MDF1_Filter0->SITFCR, (unsigned int)MDF1_Filter0->BSMXCR,
			(unsigned int)MDF1_Filter0->DFLTCR, (unsigned int)MDF1_Filter0->DFLTCICR,
			(unsigned int)MDF1_Filter0->DFLTRSFR, (unsigned int)MDF1_Filter0->DFLTISR);
}
/* Is the data line free when the microphone is not clocked, and does anything
 * clamp it? With the clock pin held still the microphone sleeps and lets go
 * of its output, so the line should follow a pull-up to 1 and a pull-down to
 * 0. A line that stays low against the pull-up is clamped by a part without
 * supply (its protection diode to a dead rail); one that ignores both pulls
 * is being driven.
 */
int mic_probe(char *buf, size_t len)
{
	int r[4];

	ok = false;
	HAL_MDF_AcqStop_DMA(&hmdf);
	HAL_MDF_DeInit(&hmdf);
	LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_2, LL_GPIO_MODE_OUTPUT);
	LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_8, LL_GPIO_MODE_INPUT);
	for (int clk = 0; clk < 2; clk++) {
		if (clk) {
			LL_GPIO_SetOutputPin(GPIOE, LL_GPIO_PIN_2);
		} else {
			LL_GPIO_ResetOutputPin(GPIOE, LL_GPIO_PIN_2);
		}
		k_msleep(clk ? 300 : 1500); /* low first, long enough for a rail fed through the clock pin to drain */
		LL_GPIO_SetPinPull(GPIOE, LL_GPIO_PIN_8, LL_GPIO_PULL_UP);
		k_msleep(50);
		r[clk * 2] = LL_GPIO_IsInputPinSet(GPIOE, LL_GPIO_PIN_8);
		LL_GPIO_SetPinPull(GPIOE, LL_GPIO_PIN_8, LL_GPIO_PULL_DOWN);
		k_msleep(50);
		r[clk * 2 + 1] = LL_GPIO_IsInputPinSet(GPIOE, LL_GPIO_PIN_8);
	}
	LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_2, LL_GPIO_MODE_ALTERNATE);
	LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_8, LL_GPIO_MODE_ALTERNATE);
	n_errors = 0;
	last_error = 0;
	ok = start() == 0;
	return snprintf(buf, len, "clock_low: pullup=%d pulldown=%d  clock_high: pullup=%d pulldown=%d  restarted=%d\n",
			r[0], r[1], r[2], r[3], ok);
}

/* The bit stream without the MDF: clock the microphone from the pin as a GPIO
 * and read the data pin `delay_ns` after each edge. Word 2k of the capture
 * buffer holds 32 bits taken after rising edges, word 2k + 1 the 32 taken after
 * the falling edges that followed, first bit in bit 0. Interrupts are off for
 * the whole capture (bits / khz ms): sound and picture stop for that long.
 */
int mic_bitbang(uint32_t kbits, uint32_t delay_ns, uint32_t khz, int pull, char *buf, size_t len)
{
	uint32_t words = MIN(kbits * 1000 / 32, (uint32_t)CAP_SAMPLES / 2);
	uint32_t half = SystemCoreClock / (2 * MAX(khz, 100u) * 1000);
	uint32_t delay = (uint32_t)((uint64_t)delay_ns * SystemCoreClock / 1000000000u);

	if (cap_buf == NULL) {
		cap_buf = psram_alloc(CAP_SAMPLES * sizeof(int32_t), 32);
	}
	if (cap_buf == NULL || delay + 8 >= half) {
		return snprintf(buf, len, "err no memory, or the delay does not fit in half a period\n");
	}
	ok = false;
	cap_left = 0;
	HAL_MDF_AcqStop_DMA(&hmdf);
	HAL_MDF_DeInit(&hmdf);
	LL_GPIO_ResetOutputPin(GPIOE, LL_GPIO_PIN_2);
	LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_2, LL_GPIO_MODE_OUTPUT);
	LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_8, LL_GPIO_MODE_INPUT);
	LL_GPIO_SetPinPull(GPIOE, LL_GPIO_PIN_8, pull == 1 ? LL_GPIO_PULL_UP : pull == 2 ? LL_GPIO_PULL_DOWN : LL_GPIO_PULL_NO);
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	unsigned int key = irq_lock();
	uint32_t t = DWT->CYCCNT;

	for (uint32_t w = 0; w < words; w++) {
		uint32_t hi = 0, lo = 0;

		for (uint32_t b = 0; b < 32; b++) {
			GPIOE->BSRR = LL_GPIO_PIN_2;
			while (DWT->CYCCNT - t < delay) {
			}
			hi |= ((GPIOE->IDR >> 8) & 1u) << b;
			t += half;
			while ((int32_t)(DWT->CYCCNT - t) < 0) {
			}
			GPIOE->BRR = LL_GPIO_PIN_2;
			while (DWT->CYCCNT - t < delay) {
			}
			lo |= ((GPIOE->IDR >> 8) & 1u) << b;
			t += half;
			while ((int32_t)(DWT->CYCCNT - t) < 0) {
			}
		}
		cap_buf[2 * w] = (int32_t)hi;
		cap_buf[2 * w + 1] = (int32_t)lo;
	}
	irq_unlock(key);

	LL_GPIO_SetPinPull(GPIOE, LL_GPIO_PIN_8, LL_GPIO_PULL_NO);
	LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_2, LL_GPIO_MODE_ALTERNATE);
	LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_8, LL_GPIO_MODE_ALTERNATE);
	n_errors = 0;
	last_error = 0;
	ok = start() == 0;
	return snprintf(buf, len, "addr=0x%08x pairs=%u core_hz=%u half_cycles=%u delay_cycles=%u restarted=%d\n",
			(unsigned int)(uintptr_t)cap_buf, (unsigned int)words, (unsigned int)SystemCoreClock,
			(unsigned int)half, (unsigned int)delay, ok);
}

/* Self-test: a square wave made with the data pin's own pulls (see mic.h). */
static bool inject_up;

static void inject_flip(struct k_timer *t)
{
	ARG_UNUSED(t);
	inject_up = !inject_up;
	LL_GPIO_SetPinPull(GPIOE, LL_GPIO_PIN_8, inject_up ? LL_GPIO_PULL_UP : LL_GPIO_PULL_DOWN);
}

static K_TIMER_DEFINE(inject_timer, inject_flip, NULL);

int mic_inject(int hz, int clock_off, int pull, char *buf, size_t len)
{
	k_timer_stop(&inject_timer);
	if (clock_off) {
		/* the clock pin still, low: the microphone sleeps; the MDF's clock runs on inside */
		LL_GPIO_ResetOutputPin(GPIOE, LL_GPIO_PIN_2);
		LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_2, LL_GPIO_MODE_OUTPUT);
	} else {
		LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_2, LL_GPIO_MODE_ALTERNATE);
	}
	LL_GPIO_SetPinPull(GPIOE, LL_GPIO_PIN_8,
			   pull == 1 ? LL_GPIO_PULL_UP : pull == 2 ? LL_GPIO_PULL_DOWN : LL_GPIO_PULL_NO);
	uint32_t half_us = 0;

	if (hz > 0) {
		half_us = 500000u / (uint32_t)CLAMP(hz, 1, 5000);
		k_timer_start(&inject_timer, K_USEC(half_us), K_USEC(half_us));
	}
	return snprintf(buf, len, "ok clock_off=%d hz=%d half_us=%u pull=%d moder_pe2=%u pupdr_pe8=%u\n", clock_off, hz,
			half_us, pull, (unsigned int)((GPIOE->MODER >> 4) & 3),
			(unsigned int)((GPIOE->PUPDR >> 16) & 3));
}

/* ---- a synthetic microphone ------------------------------------------------
 *
 * A sine through a second-order sigma-delta modulator (the loop inside a PDM
 * microphone: noise transfer (1 - z^-1)^2), one bit per word in AHBSRAM2, put
 * on the data pin by GPDMA1 channel 7 at each TIM6 update: written to BSRR (the
 * pin driven) or to PUPDR (the pin's pulls, the pin left in its MDF input mode).
 * Whole periods only, so the table loops cleanly. U13 sleeps meanwhile (clock
 * pin held low), and the MDF samples with its own clock (low-frequency master
 * mode, 1.536 MHz). The two clocks are not locked: an entry is now and then
 * read once more or once less, a little noise and no more.
 *
 * Driving the pin does not reach the MDF: on the N6 a pin set as a GPIO output
 * no longer feeds its alternate-function input (the pin toggled, IDR showed
 * it, the MDF read a still line). The pulls do, the pin staying in AF mode,
 * but their edges are slow and unequal, hence the return-to-zero mode.
 */
#define PDM_TABLE ((uint32_t *)SRAM2_AHB_BASE_S)
#define PDM_MAX   ((SRAM2_AHB_SIZE - 0x100) / sizeof(uint32_t))
#define pdm_node  ((DMA_NodeTypeDef *)(SRAM2_AHB_BASE_S + SRAM2_AHB_SIZE - 0x100))

static DMA_HandleTypeDef pdm_dma;
static DMA_QListTypeDef pdm_queue;
static bool pdm_ready, pdm_running;

/* TIM6's kernel clock, counted (divided by 8) against the core's cycle
 * counter over 1 ms: a register read's latency is then 0.03 % of the count
 * (over 50 us it was 0.5 %, and the synthetic sine came out 0.5 % sharp). */
static uint32_t tim6_hz(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	TIM6->CR1 = 0;
	TIM6->PSC = 7;
	TIM6->ARR = 0xFFFF;
	TIM6->EGR = TIM_EGR_UG;

	unsigned int key = irq_lock();

	TIM6->CR1 = TIM_CR1_CEN;
	uint32_t c0 = DWT->CYCCNT, t0 = TIM6->CNT;

	while (DWT->CYCCNT - c0 < SystemCoreClock / 1000) {
	}
	uint32_t t1 = TIM6->CNT, c1 = DWT->CYCCNT;

	irq_unlock(key);
	TIM6->CR1 = 0;
	TIM6->PSC = 0;
	return (uint32_t)((uint64_t)((t1 - t0) & 0xFFFF) * 8 * SystemCoreClock / (c1 - c0));
}

static int pdm_setup(void)
{
	DMA_NodeConfTypeDef nc = {0};

	__HAL_RCC_TIM6_CLK_ENABLE();
	__HAL_RCC_TIM6_CLK_SLEEP_ENABLE();
	__HAL_RCC_AHBSRAM2_MEM_CLK_ENABLE();
	__HAL_RCC_AHBSRAM2_MEM_CLK_SLEEP_ENABLE();
	pdm_dma.Instance = GPDMA1_Channel7;
	nc.NodeType = DMA_GPDMA_LINEAR_NODE;
	nc.Init.Request = GPDMA1_REQUEST_TIM6_UP;
	nc.Init.BlkHWRequest = DMA_BREQ_SINGLE_BURST;
	nc.Init.Direction = DMA_MEMORY_TO_PERIPH;
	nc.Init.SrcInc = DMA_SINC_INCREMENTED;
	nc.Init.DestInc = DMA_DINC_FIXED;
	nc.Init.SrcDataWidth = DMA_SRC_DATAWIDTH_WORD;
	nc.Init.DestDataWidth = DMA_DEST_DATAWIDTH_WORD;
	nc.Init.SrcBurstLength = 1;
	nc.Init.DestBurstLength = 1;
	nc.Init.Priority = DMA_LOW_PRIORITY_HIGH_WEIGHT; /* below the audio output and the microphone */
	nc.Init.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
	nc.Init.TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
	nc.DataHandlingConfig.DataExchange = DMA_EXCHANGE_NONE;
	nc.DataHandlingConfig.DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED;
	nc.TriggerConfig.TriggerPolarity = DMA_TRIG_POLARITY_MASKED;
	nc.SrcSecure = DMA_CHANNEL_SRC_SEC;
	nc.DestSecure = DMA_CHANNEL_DEST_SEC;
	nc.SrcAddress = (uint32_t)PDM_TABLE;
	nc.DstAddress = (uint32_t)&GPIOE->BSRR;
	nc.DataSize = 4;

	memset(pdm_node, 0, sizeof(*pdm_node));
	if (HAL_DMAEx_List_BuildNode(&nc, pdm_node) != HAL_OK ||
	    HAL_DMAEx_List_InsertNode_Tail(&pdm_queue, pdm_node) != HAL_OK ||
	    HAL_DMAEx_List_SetCircularMode(&pdm_queue) != HAL_OK) {
		return -EIO;
	}
	pdm_dma.InitLinkedList.Priority = DMA_LOW_PRIORITY_HIGH_WEIGHT;
	pdm_dma.InitLinkedList.LinkStepMode = DMA_LSM_FULL_EXECUTION;
	pdm_dma.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT1;
	pdm_dma.InitLinkedList.TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER;
	pdm_dma.InitLinkedList.LinkedListMode = DMA_LINKEDLIST_CIRCULAR;
	if (HAL_DMAEx_List_Init(&pdm_dma) != HAL_OK || HAL_DMAEx_List_LinkQ(&pdm_dma, &pdm_queue) != HAL_OK ||
	    HAL_DMA_ConfigChannelAttributes(&pdm_dma, DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC | DMA_CHANNEL_SRC_SEC |
							       DMA_CHANNEL_DEST_SEC) != HAL_OK) {
		return -EIO;
	}
	return 0;
}

static void pdm_stop(void)
{
	TIM6->CR1 = 0;
	TIM6->DIER = 0;
	if (pdm_running) {
		HAL_DMA_Abort(&pdm_dma);
		pdm_running = false;
	}
}

int mic_pdm(int hz, int amp_pct, int mode, int rate_khz, char *buf, size_t len)
{
	if (!pdm_ready) {
		if (pdm_setup() != 0) {
			return snprintf(buf, len, "err DMA setup\n");
		}
		pdm_ready = true;
	}
	pdm_stop();
	if (mode == 0) {
		/* back to the microphone: data pin an MDF input with no pull, clock pin the MDF's */
		LL_GPIO_SetPinPull(GPIOE, LL_GPIO_PIN_8, LL_GPIO_PULL_NO);
		LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_8, LL_GPIO_MODE_ALTERNATE);
		LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_2, LL_GPIO_MODE_ALTERNATE);
		return snprintf(buf, len, "ok off\n");
	}
	/* the DMA's rate; with return to zero (mode 3) two entries a bit */
	uint32_t rate = (uint32_t)CLAMP(rate_khz, 8, 4000) * 1000, e = mode == 3 ? 2 : 1;

	if (hz <= 0 || rate % ((uint32_t)hz * e) != 0 || rate / (uint32_t)hz > PDM_MAX) {
		return snprintf(buf, len, "err %d Hz needs a whole number of bits a period at %u entries/s, at most %u\n",
				hz, rate, (unsigned int)PDM_MAX);
	}
	uint32_t period = rate / (uint32_t)hz, n = (PDM_MAX / period) * period, bpp = period / e;
	uint32_t pupd = GPIOE->PUPDR & ~(3u << 16);
	uint32_t one = mode == 1 ? (1u << 8) : pupd | (1u << 16);       /* set PE8, or pull it up */
	uint32_t zero = mode == 1 ? (1u << 24) : pupd | (2u << 16);     /* reset PE8, or pull it down */
	float a = CLAMP(amp_pct, 0, 100) / 100.0f, s1 = 0, s2 = 0;
	uint32_t ones = 0;

	/* The loop runs one table's length to settle, then the table is taken.
	 * Return to zero: each 1 is a pulse and a 0 after it, so every 1 has one
	 * rising and one falling edge whatever its neighbours. Slow or unequal
	 * edges (the pulls' RC) then cost every pulse the same: gain and offset,
	 * not distortion. Without it a run of 1s has two edges for any length, and
	 * how many runs there are depends on the signal. */
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t k = 0; k < n / e; k++) {
			float x = a * sinf(2.0f * 3.14159265f * (float)(k % bpp) / (float)bpp);
			float y = s2 >= 0 ? 1.0f : -1.0f;

			s1 += x - y;
			s2 += s1 - y;
			if (pass == 1) {
				PDM_TABLE[k * e] = y > 0 ? one : zero;
				if (e == 2) {
					PDM_TABLE[k * e + 1] = zero;
				}
				ones += y > 0;
			}
		}
	}
	sys_cache_data_flush_range(PDM_TABLE, n * sizeof(uint32_t));

	/* the microphone asleep, the data pin ours */
	LL_GPIO_ResetOutputPin(GPIOE, LL_GPIO_PIN_2);
	LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_2, LL_GPIO_MODE_OUTPUT);
	if (mode == 1) {
		LL_GPIO_SetPinPull(GPIOE, LL_GPIO_PIN_8, LL_GPIO_PULL_NO);
		LL_GPIO_ResetOutputPin(GPIOE, LL_GPIO_PIN_8);
		LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_8, LL_GPIO_MODE_OUTPUT);
	} else {
		LL_GPIO_SetPinMode(GPIOE, LL_GPIO_PIN_8, LL_GPIO_MODE_ALTERNATE);
	}

	/* one node, the whole table, round and round */
	pdm_node->LinkRegisters[NODE_CBR1_DEFAULT_OFFSET] = n * sizeof(uint32_t);
	pdm_node->LinkRegisters[NODE_CSAR_DEFAULT_OFFSET] = (uint32_t)PDM_TABLE;
	pdm_node->LinkRegisters[NODE_CDAR_DEFAULT_OFFSET] =
		mode == 1 ? (uint32_t)&GPIOE->BSRR : (uint32_t)&GPIOE->PUPDR;
	sys_cache_data_flush_range(pdm_node, sizeof(*pdm_node));

	uint32_t f = tim6_hz(), div = (f + rate / 2) / rate;

	TIM6->PSC = 0;
	TIM6->ARR = div - 1;
	TIM6->EGR = TIM_EGR_UG;
	TIM6->SR = 0;
	if (HAL_DMAEx_List_Start(&pdm_dma) != HAL_OK) {
		return snprintf(buf, len, "err DMA start 0x%x\n", (unsigned int)HAL_DMA_GetError(&pdm_dma));
	}
	pdm_running = true;
	TIM6->DIER = TIM_DIER_UDE;
	TIM6->CR1 = TIM_CR1_CEN;
	return snprintf(buf, len,
			"ok hz=%d amp_pct=%d mode=%s entries=%u periods=%u ones_pct=%u tim_hz=%u entry_rate=%u "
			"moder_pe8=%u\n",
			hz, amp_pct, mode == 1 ? "drive" : mode == 3 ? "pulls_rz" : "pulls", (unsigned int)n,
			(unsigned int)(n / period), (unsigned int)(ones * 100 / (n / e)), (unsigned int)f,
			(unsigned int)(f / div), (unsigned int)((GPIOE->MODER >> 16) & 3));
}

/* The two pins as the MDF drives and reads them, sampled as fast as the CPU
 * can for n samples with interrupts off (a few ms): each word is the cycle
 * counter shifted left by three, bit 2 set while the data pin's pull-up is on
 * (the synthetic microphone's bit), bit 1 the data pin, bit 0 the clock pin.
 * The capture carries on underneath.
 */
int mic_scope(uint32_t n, char *buf, size_t len)
{
	if (cap_buf == NULL) {
		cap_buf = psram_alloc(CAP_SAMPLES * sizeof(int32_t), 32);
	}
	if (cap_buf == NULL) {
		return snprintf(buf, len, "err no memory\n");
	}
	n = CLAMP(n, 16, (uint32_t)CAP_SAMPLES);
	cap_left = 0;
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	uint32_t *out = (uint32_t *)cap_buf;
	unsigned int key = irq_lock();

	for (uint32_t i = 0; i < n; i++) {
		uint32_t v = GPIOE->IDR, p = GPIOE->PUPDR;

		out[i] = (DWT->CYCCNT << 3) | ((((p >> 16) & 3u) == 1u) << 2) | (((v >> 8) & 1u) << 1) | ((v >> 2) & 1u);
	}
	irq_unlock(key);
	return snprintf(buf, len, "addr=0x%08x samples=%u core_hz=%u moder=%08x afrl=%08x afrh=%08x\n",
			(unsigned int)(uintptr_t)cap_buf, (unsigned int)n, (unsigned int)SystemCoreClock,
			(unsigned int)GPIOE->MODER, (unsigned int)GPIOE->AFR[0], (unsigned int)GPIOE->AFR[1]);
}

bool mic_ok(void)
{
	return ok;
}

void mic_get_status(struct mic_status *st)
{
	st->ok = ok;
	st->total = total;
	st->blocks = n_blocks;
	st->errors = n_errors;
	st->last_error = last_error;
	st->rate_hz = rate_hz;
	st->peak = blk_peak;
	st->rms = blk_rms;
	st->dc = blk_dc;
}

uint32_t mic_total(void)
{
	return total;
}

uint32_t mic_copy(uint32_t from, int32_t *dst, uint32_t n)
{
	uint32_t end = total;

	if (ring == NULL || (int32_t)(end - from) <= 0) {
		return 0;
	}
	if (end - from > MIC_KEEP) {
		return 0; /* overwritten */
	}
	n = MIN(n, end - from);
	for (uint32_t i = 0; i < n; i++) {
		dst[i] = ring[(from + i) & (MIC_KEEP - 1)];
	}
	return n;
}

int mic_capture(uint32_t ms, char *buf, size_t len)
{
	if (ms > 0) {
		if (cap_buf == NULL) {
			cap_buf = psram_alloc(CAP_SAMPLES * sizeof(int32_t), 32);
		}
		if (cap_buf == NULL) {
			return snprintf(buf, len, "err no memory\n");
		}
		cap_left = 0; /* the microphone thread is above this one: stop it first */
		cap_fill = 0;
		cap_left = MIN(ms * (MIC_RATE / 1000), (uint32_t)CAP_SAMPLES);
	}
	return snprintf(buf, len, "addr=0x%08x frames=%u filled=%u\n", (unsigned int)(uintptr_t)cap_buf,
			(unsigned int)(cap_fill + cap_left), (unsigned int)cap_fill);
}
