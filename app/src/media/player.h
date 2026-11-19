/*
 * Video player: N6V clips (Motion JPEG + PCM), decoded by the hardware JPEG
 * codec on a worker thread. A clip is either in the media partition of the
 * XSPI flash, where it is read in place, or a file on the microSD card
 * (/SD:/clips/x.n6v), which a reader thread streams into a window of PSRAM
 * ahead of the play position.
 *
 * A clip on the card with the same title as one in flash takes that clip's
 * place in the list; the others follow the flash clips, by file name.
 *
 * Layout, all little-endian, everything 4-byte aligned:
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

#define PLAYER_MAX_CLIPS 24

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
	bool audio_clock;        /* true when the DAC sample clock is driving playback */
	uint32_t seek_us;        /* from the last seek (or the open) to its picture being decoded */
	/* clips on the card */
	bool on_card;
	bool buffering;          /* waiting for the reader: the clock is held */
	uint32_t stalls;         /* times playback ran into data not read yet (not counting seeks) */
	uint32_t ahead_kb;       /* read ahead of the frame on screen */
};

/* Scan the media partition and the card. Returns the number of clips. */
int player_init(void);
/* The card's contents changed: read the list of clips again. In two steps,
 * because reading the card takes milliseconds per clip. player_scan() reads
 * it into a staging list and may run on any thread. player_commit() puts
 * that list in place: UI thread, instant, only while no clip is open
 * (-EBUSY otherwise, try again). Clip numbers may change at the commit.
 */
int player_scan(void);
int player_commit(void);
/* Leave the card's clips out of the list, or take them in again: the flash
 * clips they stand in for come back. For testing both paths (ctl "card 0").
 */
void player_use_card(bool on);
int player_clip_count(void);
const struct n6v_header *player_clip(int clip);
bool player_clip_on_card(int clip);
/* The clip's file name on the card, "" for a clip in flash. */
const char *player_clip_file(int clip);
/* True while the player has this file in /SD:/clips open, or is about to. */
bool player_using_file(const char *name);

int player_open(int clip);
void player_close(void);

/* The attract lap's clip (media/film/attract.py): this file in /SD:/clips,
 * kept out of the list like every name that starts with an underscore. Its
 * header while the card holds it, else NULL; open it like a listed clip. */
#define PLAYER_ATTRACT_FILE "_attract.n6v"
const struct n6v_header *player_attract_clip(void);
int player_open_attract(void);
/* Where the open clip is, in ms of clip time: the sample at the DAC now (the
 * music's own position, for things that keep time with it), the frame where
 * there is no sound. 0 with no clip open. */
uint32_t player_position_ms(void);
void player_play(void);
void player_pause(void);
void player_seek(uint32_t frame);
/* A finger is on the slider (true) or has left it (false). While it is on,
 * the picture stays on the frame of the last player_seek() instead of running
 * on, and the sound is short cross-faded pieces of the clip from that frame,
 * silent while the frame does not change. Leaving resumes from that frame.
 * Call it after the first player_seek().
 */
void player_scrub(bool on);
void player_set_loop(bool loop);
/* Off: the open clip's pictures are not decoded, its clock and sound go on
 * (the attract lap's clip under the demos); on again, the frame due is
 * decoded and nothing older is shown. Opening a clip turns it on. */
void player_show_picture(bool on);
void player_get_status(struct player_status *st);
void player_note_convert(uint32_t us);

/* UI thread: the newest decoded frame as YCbCr MCU blocks, or NULL if none
 * yet. The buffer stays valid until the next call. *fresh is true when it
 * differs from the frame returned last time.
 */
const uint8_t *player_acquire(struct jpeg_info *info, uint32_t *frame, bool *fresh);

/* Decode one frame of a clip synchronously (thumbnails). Not for use while a
 * clip is open. For a clip on the card this reads the card: milliseconds.
 */
int player_decode_still(int clip, uint32_t frame, uint8_t *mcu, size_t mcu_cap, struct jpeg_info *info);

#endif
