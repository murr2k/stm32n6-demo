/*
 * Hardware JPEG decode: the STM32N6 JPEG codec produces YCbCr MCU blocks,
 * DMA2D converts them to ARGB8888.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MEDIA_JPEGDEC_H
#define MEDIA_JPEGDEC_H

#include <stddef.h>
#include <stdint.h>

struct jpeg_frame {
	uint16_t width, height;
	uint32_t decode_us;   /* JPEG codec, entropy decode + IDCT */
	uint32_t convert_us;  /* DMA2D YCbCr 4:2:0 to ARGB8888 */
};

int jpegdec_init(void);

/* Decode a baseline 4:2:0 JPEG whose dimensions are multiples of 16 into
 * `dst` (ARGB8888, dst_stride_px pixels per row). Blocking.
 */
int jpegdec_decode(const uint8_t *jpg, size_t len, uint32_t *dst, int dst_stride_px,
		   struct jpeg_frame *info);

#endif
