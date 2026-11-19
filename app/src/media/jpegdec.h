/*
 * Hardware JPEG decode: the STM32N6 JPEG codec produces YCbCr MCU blocks,
 * DMA2D converts them to ARGB8888.
 *
 * The two halves are separate calls because they run on different threads in
 * the video player: the decode on the player thread, the conversion on the UI
 * thread, straight into the frame buffer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MEDIA_JPEGDEC_H
#define MEDIA_JPEGDEC_H

#include <stddef.h>
#include <stdint.h>

struct jpeg_info {
	uint16_t width, height;
	uint8_t css;          /* DMA2D chroma sub-sampling code */
};

/* Size of the MCU buffer a frame of this geometry decodes into. */
#define JPEG_MCU_BYTES(w, h) ((size_t)((w) / 16) * ((h) / 16) * 384)

/* Zero bytes that must be readable after the JPEG data, see jpegdec_decode(). */
#define JPEG_PAD 8

int jpegdec_init(void);

/* Read width, height and sub-sampling from the JPEG header. Only baseline
 * 4:2:0 with dimensions that are multiples of 16 is accepted.
 */
int jpegdec_probe(const uint8_t *jpg, size_t len, struct jpeg_info *info);

/* Decode to YCbCr MCU blocks. `jpg` must be 4-byte aligned and followed by
 * JPEG_PAD readable zero bytes: the codec takes whole 32-bit words and needs
 * the end-of-image marker flushed through. One caller at a time.
 */
int jpegdec_decode(const uint8_t *jpg, size_t len, uint8_t *mcu, size_t mcu_cap, uint32_t *us);

/* Convert MCU blocks to ARGB8888 at `dst` (stride in pixels). The source is
 * read by DMA2D, the destination written by it; cache maintenance for both
 * is handled here.
 */
int jpegdec_convert(const uint8_t *mcu, const struct jpeg_info *info, uint32_t *dst, int stride_px,
		    uint32_t *us);

/* Copy a whole 32-bit picture of w x h pixels as it is with DMA2D, the
 * caller asleep while it runs. dst must not be in the CPU's cache: it is
 * written behind it and meant for another bus master. UI thread only.
 */
int dma2d_copy32(const void *src, void *dst, int w, int h, uint32_t *us);

/* Copy an RGB565 picture (e.g. a camera frame) to ARGB8888 at dst with
 * DMA2D. Strides in pixels. UI thread only, like jpegdec_convert(): the two
 * share the DMA2D.
 */
int dma2d_rgb565(const uint16_t *src, int src_stride_px, int w, int h, uint32_t *dst, int dst_stride_px,
		 uint32_t *us);

#endif
