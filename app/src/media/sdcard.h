/*
 * microSD card: the media library's storage.
 *
 * The card is mounted at /SD: (FAT32 or exFAT, never formatted here). Clips
 * are .n6v files in /SD:/clips. A small service on tcp/5558 lets the host put
 * them there over Ethernet (tools/n6v.py upload) instead of writing the
 * flash media partition over SWD.
 *
 * Upload protocol, text lines with binary data after "put":
 *
 *   list                        -> "f <name> <bytes>" per file, then "ok <count>"
 *   put <name> <bytes> <crc32>  -> "ok go", then the host sends the bytes,
 *                                  then "ok <bytes> <ms> <ms receiving> <ms writing>"
 *                                  or "err <why>"
 *   del <name>                  -> "ok" or "err <why>"
 *   df                          -> "ok total_mb=<n> free_mb=<n>"
 *   mount                       -> "ok" or "err <errno>" (a card put in after boot)
 *
 * A file arrives under a temporary name and is renamed only when its CRC
 * matches, so a broken upload never shows up as a clip.
 *
 * The data of a "put" goes in pieces of SD_PIECE bytes, and after each whole
 * piece the host waits for one "." from the board before it sends the next.
 * That is not flow control for its own sake. Zephyr's TCP acknowledges a
 * segment at once only when it carries PSH and otherwise after 100 ms, and a
 * host sets PSH only when its send buffer runs empty; a plain bulk send
 * therefore moves one receive window (21 KB here) per 100 ms: 0.2 MB/s
 * measured. With a pause after every piece each piece ends in a PSH.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MEDIA_SDCARD_H
#define MEDIA_SDCARD_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>

#define SD_CLIPS_DIR "/SD:/clips"
#define SD_NAME_MAX  40      /* file name with its terminator */
#define SD_IO_BLOCK  32768   /* bytes per card transfer */
#define SD_PIECE     8192    /* bytes of an upload per credit; well under the TCP receive window */
#define SD_CHANGES   4       /* how many changes of the card's contents are remembered by name */

struct sd_status {
	bool mounted;
	bool high_speed;             /* switched to High Speed (CMD6): the bus at 48 MHz, else 24 */
	bool space_known;
	uint32_t total_mb, free_mb;
	uint32_t generation;         /* goes up by one whenever the clips on the card change */
	/* the file whose arrival or removal made generation g is changed[g % SD_CHANGES] */
	char changed[SD_CHANGES][SD_NAME_MAX];
	bool receiving;
	char rx_name[SD_NAME_MAX];
	uint32_t rx_done, rx_size;   /* bytes */
	uint32_t rx_kbps;            /* of the upload in progress, or of the last one */
};

/* Mount the card. Safe to call again; returns 0 once it is mounted, -ENODEV
 * at once when the slot is empty. A thread of sdcard.c watches the card-detect
 * switch: a card taken out is unmounted (after which `generation` moves on and
 * the clips are read again), one put in is mounted. */
int sdcard_mount(void);
bool sdcard_mounted(void);
/* A card is in the slot (the detect switch). */
bool sdcard_present(void);
/* The card was seen to go and has not been mounted since: a clip playing
 * from it is to be closed, reads of it fail at once. */
bool sdcard_gone(void);
/* Testing (ctl "sdcard out|in|auto"): the watcher acts as if the card were
 * taken out or put in, or follows the detect switch again. */
enum { SD_SIM_AUTO, SD_SIM_OUT, SD_SIM_IN };
void sdcard_simulate(int mode);
int sdcard_watch_status(char *out, size_t len);
void sdcard_get_status(struct sd_status *st);

/* Read up to `len` bytes at `off` of an open file into `dst`. Returns the
 * number read (short only at the end of the file) or a negative errno.
 *
 * All reads go through one cache-line-aligned buffer in whole sectors, so
 * that the SDMMC's DMA never lands in the middle of somebody else's cache
 * line and `dst` may be anywhere (PSRAM included).
 */
int sdcard_read_at(struct fs_file_t *f, uint32_t off, void *dst, uint32_t len);

/* The clips on the card changed (or the use made of them did), because of
 * this file; "" when no one file is to blame. Bumps `generation`.
 */
void sdcard_note_change(const char *name);

/* Held while a clip file is opened, replaced or deleted. */
void sdcard_library_lock(void);
void sdcard_library_unlock(void);

/* A small settings file, written so that a card taken out at any moment
 * leaves the old contents or the new whole (via path.new). Returns 0 or a
 * negative errno (-ENODEV: no card). */
int sdcard_save(const char *path, const void *data, size_t len);
/* Read it back (up to cap bytes; finishes a save the card was taken out in
 * the middle of). Returns the bytes read or a negative errno. */
int sdcard_load(const char *path, void *buf, size_t cap);

#endif
