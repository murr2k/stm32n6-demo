/*
 * Hardware JPEG decode. See jpegdec.h.
 *
 * The codec is driven at register level. The HAL's polling decode spends
 * about 140 ns of bookkeeping per FIFO word, which made an 800x480 frame cost
 * 20 ms; the loop below only moves data.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <soc.h>
#include <stm32n6xx_hal.h>

#include "jpegdec.h"
#include "platform/gfx.h"

LOG_MODULE_REGISTER(jpegdec, LOG_LEVEL_INF);

#define DECODE_TIMEOUT_US 200000

static DMA2D_HandleTypeDef hdma2d;
static JPEG_HandleTypeDef hjpeg;

int jpegdec_init(void)
{
	/* Sleep enables too: without them these clocks stop whenever the CPU idles. */
	__HAL_RCC_JPEG_CLK_ENABLE();
	__HAL_RCC_JPEG_CLK_SLEEP_ENABLE();
	__HAL_RCC_JPEG_FORCE_RESET();
	__HAL_RCC_JPEG_RELEASE_RESET();
	__HAL_RCC_DMA2D_CLK_ENABLE();
	__HAL_RCC_DMA2D_CLK_SLEEP_ENABLE();
	__HAL_RCC_DMA2D_FORCE_RESET();
	__HAL_RCC_DMA2D_RELEASE_RESET();

	/* The HAL init loads the codec's default tables and enables the core. */
	hjpeg.Instance = JPEG;
	if (HAL_JPEG_Init(&hjpeg) != HAL_OK) {
		LOG_ERR("HAL_JPEG_Init failed");
		return -EIO;
	}
	hdma2d.Instance = DMA2D;
	return 0;
}

int jpegdec_probe(const uint8_t *jpg, size_t len, struct jpeg_info *info)
{
	size_t p = 2;

	if (len < 4 || jpg[0] != 0xff || jpg[1] != 0xd8) {
		return -EINVAL;
	}
	while (p + 4 <= len) {
		if (jpg[p] != 0xff) {
			return -EINVAL;
		}
		uint8_t marker = jpg[p + 1];
		size_t seg = ((size_t)jpg[p + 2] << 8) | jpg[p + 3];

		if (marker == 0xc0) { /* SOF0: baseline */
			if (p + 13 > len || jpg[p + 9] != 3) {
				return -ENOTSUP;
			}
			info->height = (jpg[p + 5] << 8) | jpg[p + 6];
			info->width = (jpg[p + 7] << 8) | jpg[p + 8];
			if (jpg[p + 11] != 0x22 || (info->width % 16) || (info->height % 16)) {
				return -ENOTSUP; /* luma sampling 2x2 means 4:2:0 */
			}
			info->css = DMA2D_CSS_420;
			return 0;
		}
		if (marker == 0xc2 || marker == 0xda) {
			return -ENOTSUP; /* progressive, or scan data before any SOF0 */
		}
		p += 2 + seg;
	}
	return -EINVAL;
}

int jpegdec_decode(const uint8_t *jpg, size_t len, uint8_t *mcu, size_t mcu_cap, uint32_t *us)
{
	const uint32_t *in = (const uint32_t *)jpg;
	const uint32_t *in_end = in + (len + JPEG_PAD) / 4;
	uint32_t *out = (uint32_t *)mcu;
	uint32_t *out_end = out + mcu_cap / 4;
	uint32_t t0 = gfx_now_us();
	int ret = 0;

	__ASSERT(((uintptr_t)jpg & 3) == 0, "JPEG input must be word aligned");

	JPEG->CONFR0 = 0;
	JPEG->CONFR1 |= JPEG_CONFR1_DE | JPEG_CONFR1_HDR;
	JPEG->CR = JPEG_CR_JCEN | JPEG_CR_IFF | JPEG_CR_OFF; /* no interrupts, no DMA, flush */
	JPEG->CFR = JPEG_CFR_CEOCF | JPEG_CFR_CHPDF;
	JPEG->CONFR0 = JPEG_CONFR0_START;

	for (uint32_t spins = 0;; spins++) {
		uint32_t sr = JPEG->SR;

		if (sr & JPEG_SR_OFNEF) {
			do {
				if (out == out_end) {
					ret = -ENOSPC;
					goto stop;
				}
				*out++ = JPEG->DOR;
			} while (JPEG->SR & JPEG_SR_OFNEF);
		} else if (sr & JPEG_SR_EOCF) {
			break;
		}
		if ((sr & JPEG_SR_IFNFF) && in < in_end) {
			JPEG->DIR = *in++;
		}
		if ((spins & 0xfff) == 0xfff && gfx_now_us() - t0 > DECODE_TIMEOUT_US) {
			ret = -ETIMEDOUT;
			goto stop;
		}
	}
	while ((JPEG->SR & JPEG_SR_OFNEF) && out < out_end) {
		*out++ = JPEG->DOR;
	}
stop:
	JPEG->CONFR0 = 0;
	JPEG->CFR = JPEG_CFR_CEOCF | JPEG_CFR_CHPDF;
	if (us) {
		*us = gfx_now_us() - t0;
	}
	if (ret) {
		LOG_ERR("decode failed (%d): %u of %u bytes in, %u out", ret,
			(unsigned int)((const uint8_t *)in - jpg), (unsigned int)len,
			(unsigned int)((uint8_t *)out - mcu));
	}
	return ret;
}

int jpegdec_convert(const uint8_t *mcu, const struct jpeg_info *info, uint32_t *dst, int stride_px,
		    uint32_t *us)
{
	uint32_t t0 = gfx_now_us();
	int ret = 0;

	hdma2d.Init.Mode = DMA2D_M2M_PFC;
	hdma2d.Init.ColorMode = DMA2D_OUTPUT_ARGB8888;
	hdma2d.Init.OutputOffset = stride_px - info->width;
	hdma2d.Init.AlphaInverted = DMA2D_REGULAR_ALPHA;
	hdma2d.Init.RedBlueSwap = DMA2D_RB_REGULAR;
	hdma2d.Init.BytesSwap = DMA2D_BYTES_REGULAR;
	hdma2d.Init.LineOffsetMode = DMA2D_LOM_PIXELS;

	hdma2d.LayerCfg[1].AlphaMode = DMA2D_REPLACE_ALPHA;
	hdma2d.LayerCfg[1].InputAlpha = 0xFF;
	hdma2d.LayerCfg[1].InputColorMode = DMA2D_INPUT_YCBCR;
	hdma2d.LayerCfg[1].ChromaSubSampling = info->css;
	hdma2d.LayerCfg[1].InputOffset = 0;
	hdma2d.LayerCfg[1].RedBlueSwap = DMA2D_RB_REGULAR;
	hdma2d.LayerCfg[1].AlphaInverted = DMA2D_REGULAR_ALPHA;

	sys_cache_data_flush_range((void *)mcu, JPEG_MCU_BYTES(info->width, info->height));
	/* The destination may hold dirty lines from earlier drawing: write them
	 * out first, or a later eviction would overwrite what DMA2D produced.
	 */
	sys_cache_data_flush_and_invd_range(dst, (size_t)stride_px * info->height * 4);

	if (HAL_DMA2D_Init(&hdma2d) != HAL_OK || HAL_DMA2D_ConfigLayer(&hdma2d, 1) != HAL_OK ||
	    HAL_DMA2D_Start(&hdma2d, (uint32_t)mcu, (uint32_t)dst, info->width, info->height) != HAL_OK ||
	    HAL_DMA2D_PollForTransfer(&hdma2d, 100) != HAL_OK) {
		ret = -EIO;
	}
	sys_cache_data_invd_range(dst, (size_t)stride_px * info->height * 4);
	if (us) {
		*us = gfx_now_us() - t0;
	}
	return ret;
}
