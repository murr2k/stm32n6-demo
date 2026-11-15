/*
 * Video player: N6V clips (Motion JPEG + PCM) stored in the media partition
 * of the XSPI flash, decoded by the hardware JPEG codec on a worker thread.
 *
 * Flash layout, all little-endian, everything 4-byte aligned:
 *
 *   media partition (FLASH + 0x42_0000):
 *     "N6MD", u32 count, count x { u32 offset, u32 size }   offsets from the partition start
 *   clip (an .n6v file):
 *     struct n6v_header (64 bytes)
 *     n_frames x chunk: u32 jpeg_len, u32 audio_len,
 *                       jpeg bytes + JPEG_PAD zeros, padded to 4,
 *                       audio bytes (s16le interleaved), padded to 4
 *     n_frames x u32 chunk offset, from the clip start
 *
 * Motion JPEG is intra-only, so seeking is a table lookup and any frame can be
 * decoded on its own.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MEDIA_PLAYER_H
#define MEDIA_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpegdec.h"

struct n6v_header {
	char magic[4];           /* "N6V1" */
	uint16_t width, height;
	uint16_t fps_num, fps_den;
	uint32_t n_frames;
	uint32_t audio_rate;
	uint16_t audio_channels;
	uint16_t audio_bits;
	uint32_t index_off;
	uint32_t file_size;
	char title[32];
};

struct player_status {
	bool open, playing;
	uint32_t frame, n_frames;
	uint32_t decode_us;      /* last frame */
	uint32_t convert_us;     /* last frame, set by the UI via player_note_convert() */
	uint32_t dropped;        /* frames skipped because decoding fell behind */
	uint32_t kbps;           /* video bit rate over the last second of clip time */
	uint32_t frame_bytes;    /* compressed size of the last frame */
};

/* Scan the media partition. Returns the number of clips found (0 if none). */
int player_init(void);
int player_clip_count(void);
const struct n6v_header *player_clip(int clip);

int player_open(int clip);
void player_close(void);
void player_play(void);
void player_pause(void);
void player_seek(uint32_t frame);
void player_set_loop(bool loop);
void player_get_status(struct player_status *st);
void player_note_convert(uint32_t us);

/* UI thread: the newest decoded frame as YCbCr MCU blocks, or NULL if none
 * yet. The buffer stays valid until the next call. *fresh is true when it
 * differs from the frame returned last time.
 */
const uint8_t *player_acquire(struct jpeg_info *info, uint32_t *frame, bool *fresh);

/* Decode one frame of a clip synchronously (thumbnails). Not for use while a
 * clip is open.
 */
int player_decode_still(int clip, uint32_t frame, uint8_t *mcu, size_t mcu_cap, struct jpeg_info *info);

#endif
