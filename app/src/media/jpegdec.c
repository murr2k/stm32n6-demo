/*
 * Hardware JPEG decode. See jpegdec.h.
 *
 * Polling mode for now: the CPU feeds the codec's input FIFO and drains its
 * output FIFO. That is enough to prove the path and measure it; the video
 * player will move both FIFOs to DMA.
 *
 * Cache discipline: the MCU buffer is written by the CPU and read by DMA2D,
 * so it is flushed before the conversion. The ARGB output is written by
 * DMA2D, so the CPU's cached view of it is invalidated afterwards.
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
#include "platform/psram.h"

LOG_MODULE_REGISTER(jpegdec, LOG_LEVEL_INF);

/* 4:2:0: a 16x16 pixel MCU is four Y blocks, one Cb and one Cr: 384 bytes. */
#define MCU_BYTES_420 384
#define MAX_W         800
#define MAX_H         480
#define MCU_BUF_BYTES ((MAX_W / 16) * (MAX_H / 16) * MCU_BYTES_420)
/* Largest compressed frame accepted, and the zero padding appended to it. The
 * HAL feeds the codec whole 32-bit words: a file whose length is not a
 * multiple of four leaves the end-of-image marker unsent and the decode never
 * completes. Padding after the marker is ignored by the codec.
 */
#define IN_MAX_BYTES  (256 * 1024)
#define IN_PAD_BYTES  32

static JPEG_HandleTypeDef hjpeg;
static DMA2D_HandleTypeDef hdma2d;
static uint8_t *mcu_buf;
static uint8_t *in_buf;
static K_MUTEX_DEFINE(lock);

/* The HAL calls these when its buffers run dry or fill up. With the whole
 * file and the whole frame handed over in one call, neither is expected to
 * do anything, but pausing is the safe response if they ever fire.
 */
void HAL_JPEG_GetDataCallback(JPEG_HandleTypeDef *h, uint32_t n_decoded)
{
	ARG_UNUSED(n_decoded);
	HAL_JPEG_ConfigInputBuffer(h, NULL, 0);
}

void HAL_JPEG_DataReadyCallback(JPEG_HandleTypeDef *h, uint8_t *out, uint32_t len)
{
	ARG_UNUSED(h);
	ARG_UNUSED(out);
	ARG_UNUSED(len);
}

int jpegdec_init(void)
{
	mcu_buf = psram_alloc(MCU_BUF_BYTES, 32);
	in_buf = psram_alloc(IN_MAX_BYTES + IN_PAD_BYTES, 32);
	if (mcu_buf == NULL || in_buf == NULL) {
		return -ENOMEM;
	}
	__HAL_RCC_JPEG_CLK_ENABLE();
	__HAL_RCC_JPEG_FORCE_RESET();
	__HAL_RCC_JPEG_RELEASE_RESET();
	__HAL_RCC_DMA2D_CLK_ENABLE();
	__HAL_RCC_DMA2D_FORCE_RESET();
	__HAL_RCC_DMA2D_RELEASE_RESET();

	hjpeg.Instance = JPEG;
	if (HAL_JPEG_Init(&hjpeg) != HAL_OK) {
		LOG_ERR("HAL_JPEG_Init failed");
		return -EIO;
	}
	hdma2d.Instance = DMA2D;
	return 0;
}

static int convert(uint32_t *dst, int dst_stride_px, int w, int h)
{
	hdma2d.Init.Mode = DMA2D_M2M_PFC;
	hdma2d.Init.ColorMode = DMA2D_OUTPUT_ARGB8888;
	hdma2d.Init.OutputOffset = dst_stride_px - w;
	hdma2d.Init.AlphaInverted = DMA2D_REGULAR_ALPHA;
	hdma2d.Init.RedBlueSwap = DMA2D_RB_REGULAR;
	hdma2d.Init.BytesSwap = DMA2D_BYTES_REGULAR;
	hdma2d.Init.LineOffsetMode = DMA2D_LOM_PIXELS;
	hdma2d.XferCpltCallback = NULL;
	hdma2d.XferErrorCallback = NULL;

	hdma2d.LayerCfg[1].AlphaMode = DMA2D_REPLACE_ALPHA;
	hdma2d.LayerCfg[1].InputAlpha = 0xFF;
	hdma2d.LayerCfg[1].InputColorMode = DMA2D_INPUT_YCBCR;
	hdma2d.LayerCfg[1].ChromaSubSampling = DMA2D_CSS_420;
	hdma2d.LayerCfg[1].InputOffset = 0;
	hdma2d.LayerCfg[1].RedBlueSwap = DMA2D_RB_REGULAR;
	hdma2d.LayerCfg[1].AlphaInverted = DMA2D_REGULAR_ALPHA;

	if (HAL_DMA2D_Init(&hdma2d) != HAL_OK || HAL_DMA2D_ConfigLayer(&hdma2d, 1) != HAL_OK) {
		return -EIO;
	}
	if (HAL_DMA2D_Start(&hdma2d, (uint32_t)mcu_buf, (uint32_t)dst, w, h) != HAL_OK ||
	    HAL_DMA2D_PollForTransfer(&hdma2d, 100) != HAL_OK) {
		return -EIO;
	}
	return 0;
}

int jpegdec_decode(const uint8_t *jpg, size_t len, uint32_t *dst, int dst_stride_px,
		   struct jpeg_frame *info)
{
	JPEG_ConfTypeDef conf;
	uint32_t t0;
	int ret = 0;

	if (len > IN_MAX_BYTES) {
		return -E2BIG;
	}
	k_mutex_lock(&lock, K_FOREVER);
	t0 = gfx_now_us();
	memcpy(in_buf, jpg, len);
	memset(in_buf + len, 0, IN_PAD_BYTES);
	if (HAL_JPEG_Decode(&hjpeg, in_buf, ROUND_UP(len, 4) + IN_PAD_BYTES - 4, mcu_buf, MCU_BUF_BYTES, 500) !=
	    HAL_OK) {
		LOG_ERR("decode failed, HAL error 0x%x", (unsigned int)HAL_JPEG_GetError(&hjpeg));
		LOG_ERR("in %u/%u out %u, CONFR0 %08x CONFR1 %08x CR %08x SR %08x AHB5ENR %08x",
			(unsigned int)hjpeg.JpegInCount, (unsigned int)len, (unsigned int)hjpeg.JpegOutCount,
			(unsigned int)JPEG->CONFR0, (unsigned int)JPEG->CONFR1, (unsigned int)JPEG->CR,
			(unsigned int)JPEG->SR, (unsigned int)RCC->AHB5ENR);
		ret = -EIO;
		goto out;
	}
	info->decode_us = gfx_now_us() - t0;

	HAL_JPEG_GetInfo(&hjpeg, &conf);
	info->width = conf.ImageWidth;
	info->height = conf.ImageHeight;
	if (conf.ChromaSubsampling != JPEG_420_SUBSAMPLING || conf.ImageWidth > MAX_W ||
	    conf.ImageHeight > MAX_H || (conf.ImageWidth % 16) || (conf.ImageHeight % 16)) {
		LOG_ERR("unsupported JPEG: %ux%u, subsampling %u", (unsigned int)conf.ImageWidth,
			(unsigned int)conf.ImageHeight, (unsigned int)conf.ChromaSubsampling);
		ret = -ENOTSUP;
		goto out;
	}

	t0 = gfx_now_us();
	sys_cache_data_flush_range(mcu_buf, (conf.ImageWidth / 16) * (conf.ImageHeight / 16) * MCU_BYTES_420);
	ret = convert(dst, dst_stride_px, conf.ImageWidth, conf.ImageHeight);
	sys_cache_data_invd_range(dst, (size_t)dst_stride_px * conf.ImageHeight * 4);
	info->convert_us = gfx_now_us() - t0;
out:
	k_mutex_unlock(&lock);
	return ret;
}
