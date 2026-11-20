/*
 * Video player. See player.h.
 *
 * Threading: the worker owns the JPEG codec and decodes into one of three MCU
 * buffers in PSRAM. Three, so that it can always find one that is neither the
 * newest published frame nor the one the UI thread is converting.
 *
 * A clip in flash is read in place. A clip on the card is streamed by a third
 * thread, the reader, into a ring of PSRAM that holds a window [lo, hi) of
 * the file around the play position. Only the reader moves the window; the
 * worker and the audio thread copy out of it under `win_lock`, and where
 * what they want is not there yet they wait: the worker holds the clock, the
 * audio thread hands over silence without advancing. So a slow card shows as
 * a pause, never as skipped frames or a wrong picture.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "platform/audio.h"
#include "platform/gfx.h"
#include "platform/psram.h"
#include "player.h"
#include "sdcard.h"

LOG_MODULE_REGISTER(player, LOG_LEVEL_INF);

#define FLASH_BASE_ADDR 0x70000000u
#define MEDIA_OFFSET    0x00420000u   /* first byte after the stock Zephyr partitions */
#define FLASH_MAX_CLIPS 8
#define NBUF            3
#define MCU_CAP         JPEG_MCU_BYTES(800, 480)

#define WIN_BYTES MB(4)               /* read-ahead ring; a power of two */
#define WIN_BACK  KB(512)             /* kept behind the play position, for short steps back */
#define STAGE_CAP KB(256)             /* the largest JPEG frame accepted from a file */
#define INDEX_MAX (MB(1) / 4)         /* frames per clip on the card: 2.4 hours at 30 fps */
#define PATH_LEN  (sizeof(SD_CLIPS_DIR) + 1 + SD_NAME_MAX)

struct media_dir {
	char magic[4];
	uint32_t count;
	struct {
		uint32_t offset, size;
	} clip[FLASH_MAX_CLIPS];
};

struct clip {
	struct n6v_header hdr;
	const uint8_t *mem;           /* the clip in flash, or NULL: a file on the card */
	char name[SD_NAME_MAX];       /* that file's name in SD_CLIPS_DIR */
};

static struct clip clips[PLAYER_MAX_CLIPS];
static int n_clips;
static struct clip lap_clip, staged_lap;  /* PLAYER_ATTRACT_FILE, outside the list */
static bool have_lap, staged_have_lap;
static const uint8_t *flash_clip[FLASH_MAX_CLIPS];
static int n_flash;

static uint8_t *bufs[NBUF];
static struct k_spinlock lock;

/* state shared with the UI thread, under `lock` */
static int published = -1;      /* buffer holding the newest decoded frame */
static int held = -1;           /* buffer the UI is using */
static uint32_t published_frame;
static struct jpeg_info published_info;
static uint32_t last_acquired = UINT32_MAX;

static const struct clip *cur;
static volatile bool playing, loop_clip;
static uint32_t base_frame;     /* clip position at base_us */
static uint32_t base_us;
/* Audio: a_pos is the next sample frame to hand to the DAC pipeline. When the
 * clip has 48 kHz stereo sound and the codec is up, the DAC is the clock.
 */
static volatile uint32_t a_pos;
static bool use_audio;
static uint32_t spf;            /* audio sample frames per video frame */
static uint32_t decoded_frame = UINT32_MAX;
static volatile uint32_t jump_gen; /* counts position changes (seek, play, loop, open) */
static volatile bool close_req;    /* player_close() was called: the worker lets go of the clip */
static volatile bool scrub_on;     /* a finger is on the slider: see player_scrub() */
static volatile bool picture_on = true; /* see player_show_picture() */
#define GRAIN 4096u                /* sample frames of sound per scrub grain: 85 ms */
static int16_t *grain_pcm[2];      /* in PSRAM; the one not playing takes the next grain */
static volatile bool seek_pending; /* a seek (or the open) is waiting for its picture */
static uint32_t seek_frame = UINT32_MAX, seek_t0;
static struct player_status status;
static K_SEM_DEFINE(wake, 0, 1);
static K_MUTEX_DEFINE(codec);

/* The clip on the card that is open. Written by the reader thread only;
 * `clip`, `ready`, `failed`, `lo`, `hi` and the ring's contents under win_lock.
 */
static struct {
	struct fs_file_t file;
	bool file_open;
	const struct clip *clip;    /* whose file, index and window these are */
	bool ready;                 /* the index is loaded */
	bool failed;                /* the card stopped answering, or the file is not the clip listed */
	uint32_t *index;            /* chunk offsets */
	uint8_t *ring;
	uint8_t *stage;             /* one JPEG, contiguous and padded, for the codec (under `codec`) */
	uint32_t lo, hi;            /* file offsets in the ring: [lo, hi), whole sectors */
	uint32_t epoch;             /* counts restarts of the window (seek outside it, open, close) */
	uint32_t end;               /* end of the chunk data */
} sd;
static K_MUTEX_DEFINE(win_lock);
static K_SEM_DEFINE(io_wake, 0, 1);
static const struct clip *volatile io_want;   /* the clip the reader should have open */
static volatile uint32_t need_frame;          /* the frame the worker is at */

/* ---- the list of clips -------------------------------------------------- */

static bool header_ok(const struct n6v_header *h)
{
	return memcmp(h->magic, "N6V1", 4) == 0 && h->width <= 800 && h->height <= 480 && h->n_frames > 0 &&
	       h->fps_den > 0 && h->fps_num >= h->fps_den && h->index_off >= sizeof(*h);
}

static void set_clip(struct clip *c, const struct n6v_header *h, const uint8_t *mem, const char *name)
{
	c->hdr = *h;
	c->hdr.title[sizeof(c->hdr.title) - 1] = '\0';
	c->mem = mem;
	strncpy(c->name, name, sizeof(c->name) - 1);
	c->name[sizeof(c->name) - 1] = '\0';
}

static void scan_flash(void)
{
	const struct media_dir *dir = (const struct media_dir *)(FLASH_BASE_ADDR + MEDIA_OFFSET);

	if (memcmp(dir->magic, "N6MD", 4) != 0 || dir->count > FLASH_MAX_CLIPS) {
		LOG_WRN("no media directory in flash at 0x%08x", FLASH_BASE_ADDR + MEDIA_OFFSET);
		return;
	}
	for (uint32_t i = 0; i < dir->count; i++) {
		const struct n6v_header *h = (const struct n6v_header *)((const uint8_t *)dir + dir->clip[i].offset);

		if (!header_ok(h)) {
			LOG_WRN("flash clip %u: bad header, skipped", i);
			continue;
		}
		flash_clip[n_flash++] = (const uint8_t *)h;
		LOG_INF("flash clip %d: \"%s\" %ux%u, %u frames at %u/%u fps, %u KB", n_flash - 1, h->title,
			h->width, h->height, h->n_frames, h->fps_num, h->fps_den, h->file_size / 1024);
	}
}

static volatile bool use_card = true;

void player_use_card(bool on)
{
	use_card = on;
	sdcard_note_change(""); /* the list is read again */
}

/* Add the card's clips to a list that holds the flash clips so far. */
static void scan_card(struct clip *clips, int *count)
{
	static struct fs_dirent ent;
	struct fs_dir_t dir;
	int n_clips = *count, first = n_clips, found = 0;

	if (!sdcard_mounted() || sdcard_gone() || !sdcard_present() || sd.ring == NULL || !use_card) {
		return; /* gone or on its way: every header would read as broken */
	}
	fs_dir_t_init(&dir);
	sdcard_library_lock();
	if (fs_opendir(&dir, SD_CLIPS_DIR) != 0) {
		sdcard_library_unlock();
		return;
	}
	while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
		size_t len = strlen(ent.name);
		struct n6v_header h;
		struct fs_file_t f;
		char path[PATH_LEN];
		int slot = -1;

		if (ent.type != FS_DIR_ENTRY_FILE || len < 5 || len >= SD_NAME_MAX ||
		    strcasecmp(ent.name + len - 4, ".n6v") != 0) {
			continue;
		}
		bool lap = strcasecmp(ent.name, PLAYER_ATTRACT_FILE) == 0;

		if (ent.name[0] == '_' && !lap) {
			continue; /* a leading underscore keeps a file out of the list */
		}
		snprintf(path, sizeof(path), "%s/%s", SD_CLIPS_DIR, ent.name);
		fs_file_t_init(&f);
		if (fs_open(&f, path, FS_O_READ) != 0) {
			continue;
		}
		int got = sdcard_read_at(&f, 0, &h, sizeof(h));

		fs_close(&f);
		if (got != sizeof(h) || !header_ok(&h) || h.n_frames > INDEX_MAX || h.index_off > ent.size ||
		    (uint64_t)h.n_frames * 4 > ent.size - h.index_off) {
			LOG_WRN("%s: not a usable clip, skipped", ent.name);
			continue;
		}
		if (lap) {
			set_clip(&staged_lap, &h, NULL, ent.name);
			staged_have_lap = true;
			continue;
		}
		/* the same title as a clip in flash: the card's copy takes its place */
		for (int i = 0; i < n_flash; i++) {
			if (clips[i].mem && strncmp(clips[i].hdr.title, h.title, sizeof(h.title) - 1) == 0) {
				slot = i;
			}
		}
		if (slot < 0) {
			if (n_clips == PLAYER_MAX_CLIPS) {
				LOG_WRN("%s: the list is full, skipped", ent.name);
				continue;
			}
			slot = n_clips++;
		}
		set_clip(&clips[slot], &h, NULL, ent.name);
		found++;
	}
	fs_closedir(&dir);
	sdcard_library_unlock();

	for (int i = first + 1; i < n_clips; i++) { /* the card's own clips by name: directory order is arbitrary */
		struct clip c = clips[i];
		int j = i;

		for (; j > first && strcasecmp(clips[j - 1].name, c.name) > 0; j--) {
			clips[j] = clips[j - 1];
		}
		clips[j] = c;
	}
	*count = n_clips;
	LOG_INF("card: %d clip(s)", found);
}

/* Reading the card takes milliseconds per clip, too long for the UI thread
 * once the demo is running. So the list is read into `staged` by whoever has
 * the time (player_scan, any thread) and put in place by the UI thread
 * (player_commit) when no clip is open.
 */
static struct clip staged[PLAYER_MAX_CLIPS];
static int n_staged;

int player_scan(void)
{
	int n = 0;

	for (int i = 0; i < n_flash; i++) {
		set_clip(&staged[n++], (const struct n6v_header *)flash_clip[i], flash_clip[i], "");
	}
	staged_have_lap = false;
	scan_card(staged, &n);
	n_staged = n;
	return n;
}

int player_commit(void)
{
	if (cur != NULL) {
		return -EBUSY;
	}
	if (sd.clip != NULL) {
		/* the reader still holds the last clip's file and index: have it let go, try again later */
		io_want = NULL;
		k_sem_give(&io_wake);
		return -EBUSY;
	}
	memcpy(clips, staged, sizeof(clips));
	n_clips = n_staged;
	lap_clip = staged_lap;
	have_lap = staged_have_lap;
	return n_clips;
}

const struct n6v_header *player_attract_clip(void)
{
	return have_lap && use_card ? &lap_clip.hdr : NULL;
}

int player_init(void)
{
	if (bufs[0] != NULL) {
		return n_clips; /* already scanned */
	}
	for (int i = 0; i < NBUF; i++) {
		bufs[i] = psram_alloc(MCU_CAP, 32);
		if (bufs[i] == NULL) {
			return 0;
		}
	}
	if (jpegdec_init() != 0) {
		return 0;
	}
	grain_pcm[0] = psram_alloc(GRAIN * 4, 32);
	grain_pcm[1] = psram_alloc(GRAIN * 4, 32);
	sd.index = psram_alloc(INDEX_MAX * 4, 32);
	sd.stage = psram_alloc(STAGE_CAP, 32);
	sd.ring = psram_alloc(WIN_BYTES, 32);
	if (sd.ring == NULL) {
		LOG_WRN("no memory for the read-ahead window: clips on the card are off");
	} else {
		sdcard_mount();
	}
	scan_flash();
	player_scan();
	return player_commit();
}

int player_clip_count(void)
{
	return n_clips;
}

const struct n6v_header *player_clip(int clip)
{
	return (clip >= 0 && clip < n_clips) ? &clips[clip].hdr : NULL;
}

bool player_clip_on_card(int clip)
{
	return clip >= 0 && clip < n_clips && clips[clip].mem == NULL;
}

const char *player_clip_file(int clip)
{
	return (clip >= 0 && clip < n_clips) ? clips[clip].name : "";
}

bool player_using_file(const char *name)
{
	const struct clip *a = io_want, *b = sd.clip;

	return (a && a->mem == NULL && strcmp(a->name, name) == 0) ||
	       (b && b->mem == NULL && strcmp(b->name, name) == 0);
}

/* ---- clips in flash: read in place -------------------------------------- */

static const uint32_t *chunk_ptr(const struct clip *c, uint32_t frame)
{
	const uint32_t *index = (const uint32_t *)(c->mem + c->hdr.index_off);

	return (const uint32_t *)(c->mem + index[frame]);
}

static const uint8_t *chunk_audio(const struct clip *c, uint32_t frame, uint32_t *len)
{
	const uint32_t *chunk = chunk_ptr(c, frame);

	*len = chunk[1];
	return (const uint8_t *)&chunk[2] + ROUND_UP(chunk[0] + JPEG_PAD, 4);
}

static int decode_frame(const struct clip *c, uint32_t frame, uint8_t *mcu, struct jpeg_info *info,
			uint32_t *us, uint32_t *bytes)
{
	const uint32_t *chunk = chunk_ptr(c, frame);
	uint32_t jpeg_len = chunk[0];
	const uint8_t *jpg = (const uint8_t *)&chunk[2];
	int ret = jpegdec_probe(jpg, jpeg_len, info);

	if (ret == 0) {
		ret = jpegdec_decode(jpg, jpeg_len, mcu, MCU_CAP, us);
	}
	if (bytes) {
		*bytes = jpeg_len;
	}
	return ret;
}

/* ---- clips on the card: the window -------------------------------------- */

/* Copy [off, off + len) of the file out of the window. False if the window
 * does not hold all of it (yet).
 */
static bool win_get(const struct clip *c, uint32_t off, void *dst, uint32_t len)
{
	bool ok = false;

	k_mutex_lock(&win_lock, K_FOREVER);
	if (sd.clip == c && sd.ready && off >= sd.lo && off + len >= off && off + len <= sd.hi) {
		uint32_t at = off & (WIN_BYTES - 1), first = MIN(len, WIN_BYTES - at);

		memcpy(dst, sd.ring + at, first);
		memcpy((uint8_t *)dst + first, sd.ring, len - first);
		ok = true;
	}
	k_mutex_unlock(&win_lock);
	return ok;
}

/* Where chunk `frame` lies in the file. Call with win_lock held and the
 * index loaded. False if the index entry makes no sense.
 */
static bool chunk_span(const struct clip *c, uint32_t frame, uint32_t *off, uint32_t *end)
{
	*off = sd.index[frame];
	*end = frame + 1 < c->hdr.n_frames ? sd.index[frame + 1] : sd.end;
	return *off >= sizeof(struct n6v_header) && (*off & 3) == 0 && *end <= sd.end && *end >= *off + 8;
}

/* Put frame `frame`'s JPEG in sd.stage. 0, -EAGAIN while the reader has not
 * got there, -EIO when the card failed, -EBADMSG for a chunk that makes no sense.
 */
static int stage_frame(const struct clip *c, uint32_t frame, uint32_t *jpeg_len, uint32_t *ahead)
{
	uint32_t head[2], off = 0, end, body = 0, epoch;
	int ret = -EAGAIN;

	k_mutex_lock(&win_lock, K_FOREVER);
	epoch = sd.epoch;
	if (sd.clip == c && sd.failed) {
		ret = -EIO;
	} else if (sd.clip == c && sd.ready) {
		if (!chunk_span(c, frame, &off, &end)) {
			ret = -EBADMSG;
		} else if (win_get(c, off, head, 8)) {
			body = head[0] <= STAGE_CAP - JPEG_PAD - 4 ? ROUND_UP(head[0] + JPEG_PAD, 4) : UINT32_MAX;
			if (head[0] < 4 || body > end - off - 8) {
				ret = -EBADMSG;
			} else if (off + 8 + body <= sd.hi) {
				ret = 1; /* all there */
			}
		}
	}
	k_mutex_unlock(&win_lock);
	if (ret != 1) {
		return ret;
	}

	/* The JPEG, up to 250 KB, is copied without the lock: the audio thread
	 * needs that lock every 10 ms, and while it waited for this copy the
	 * worker ran at the audio thread's priority, above the UI. Safe, because
	 * the reader only writes ring space outside the window and does not move
	 * the window's tail past the frame the worker is at; the one thing that
	 * can pull the data away is a restart of the window, and the epoch tells.
	 */
	uint32_t at = (off + 8) & (WIN_BYTES - 1), first = MIN(body, WIN_BYTES - at);

	memcpy(sd.stage, sd.ring + at, first);
	memcpy(sd.stage + first, sd.ring, body - first);

	k_mutex_lock(&win_lock, K_FOREVER);
	if (sd.clip == c && sd.ready && sd.epoch == epoch && off >= sd.lo) {
		*jpeg_len = head[0];
		*ahead = sd.hi - (off + 8 + body);
		ret = 0;
	} else {
		ret = -EAGAIN;
	}
	k_mutex_unlock(&win_lock);
	return ret;
}

/* Where frame `frame`'s PCM lies in the file. False while the reader has not
 * got there. A chunk that makes no sense counts as having no sound, so that
 * the clock moves on.
 */
static bool file_pcm(const struct clip *c, uint32_t frame, uint32_t *pcm_off, uint32_t *len)
{
	uint32_t head[2], off, end;
	bool ok = false;

	k_mutex_lock(&win_lock, K_FOREVER);
	if (sd.clip == c && sd.ready && !sd.failed) {
		*len = 0;
		*pcm_off = 0;
		if (!chunk_span(c, frame, &off, &end)) {
			ok = true;
		} else if (win_get(c, off, head, 8)) {
			uint32_t body = head[0] <= STAGE_CAP ? ROUND_UP(head[0] + JPEG_PAD, 4) : UINT32_MAX;

			if (body <= end - off - 8) {
				*pcm_off = off + 8 + body;
				*len = MIN(head[1], end - *pcm_off);
			}
			ok = true;
		}
	}
	k_mutex_unlock(&win_lock);
	return ok;
}

/* ---- clips on the card: the reader thread ------------------------------- */

static void file_close(void)
{
	k_mutex_lock(&win_lock, K_FOREVER);
	sd.clip = NULL;
	sd.ready = false;
	sd.failed = false;
	sd.lo = sd.hi = 0;
	sd.epoch++;
	k_mutex_unlock(&win_lock);
	if (sd.file_open) {
		sdcard_library_lock(); /* not while a card taken out is being let go of */
		fs_close(&sd.file);
		sdcard_library_unlock();
		sd.file_open = false;
	}
}

static void file_open(const struct clip *c)
{
	struct n6v_header h;
	char path[PATH_LEN];
	int ret;

	snprintf(path, sizeof(path), "%s/%s", SD_CLIPS_DIR, c->name);
	fs_file_t_init(&sd.file);
	sdcard_library_lock();
	/* the card may have been taken out since the list was read */
	ret = sdcard_mounted() && !sdcard_gone() ? fs_open(&sd.file, path, FS_O_READ) : -ENODEV;
	sdcard_library_unlock();
	if (ret == 0) {
		sd.file_open = true;
		/* the file may have been replaced since the list was read */
		if (sdcard_read_at(&sd.file, 0, &h, sizeof(h)) != sizeof(h) ||
		    memcmp(&h, &c->hdr, offsetof(struct n6v_header, title)) != 0) {
			ret = -ESTALE;
		} else if (sdcard_read_at(&sd.file, c->hdr.index_off, sd.index, c->hdr.n_frames * 4) !=
			   (int)(c->hdr.n_frames * 4)) {
			ret = -EIO;
		}
	}
	if (ret != 0) {
		LOG_WRN("%s: cannot open (%d)", c->name, ret);
	}
	k_mutex_lock(&win_lock, K_FOREVER);
	sd.clip = c;
	sd.end = c->hdr.index_off;
	sd.lo = sd.hi = 0;
	sd.ready = ret == 0;
	sd.failed = ret != 0;
	k_mutex_unlock(&win_lock);
}

/* Read the next block ahead of the play position. False when there is
 * nothing to do: everything up to the end is held, or the window is full.
 */
static bool file_fill(void)
{
	const struct clip *c = sd.clip;
	uint32_t need = MIN(sd.index[MIN(need_frame, c->hdr.n_frames - 1)], sd.end) & ~511u;
	uint32_t end = ROUND_UP(sd.end, 512);
	uint32_t at, n;

	k_mutex_lock(&win_lock, K_FOREVER);
	if (need < sd.lo || need > sd.hi) {
		sd.lo = sd.hi = need; /* a seek to somewhere not held: start again there */
		sd.epoch++;
	}
	at = sd.hi;
	n = MIN(SD_IO_BLOCK, end - at);
	if (n > 0 && at + n - sd.lo > WIN_BYTES) {
		/* full: let go of the oldest data, but not of what the player may step back to */
		uint32_t lo = at + n - WIN_BYTES;

		if (lo > (need > WIN_BACK ? need - WIN_BACK : 0)) {
			n = 0;
		} else {
			sd.lo = lo;
		}
	}
	k_mutex_unlock(&win_lock);
	if (n == 0) {
		return false;
	}

	/* the ring space for [at, at + n) is outside the window now: no lock needed to fill it */
	uint32_t pos = at & (WIN_BYTES - 1), first = MIN(n, WIN_BYTES - pos);
	uint32_t must = MIN(n, sd.end - at); /* past the chunk data a short read is fine */
	int got = sdcard_read_at(&sd.file, at, sd.ring + pos, first);

	if (got == (int)first && first < n) {
		int more = sdcard_read_at(&sd.file, at + first, sd.ring, n - first);

		got = more < 0 ? more : got + more;
	}
	k_mutex_lock(&win_lock, K_FOREVER);
	if (got < (int)must) {
		if (sdcard_gone() || !sdcard_present()) {
			LOG_INF("%s: the card was taken out", c->name);
		} else {
			LOG_ERR("%s: read failed at %u (%d)", c->name, at, got);
		}
		sd.failed = true;
	} else {
		sd.hi = at + n;
	}
	k_mutex_unlock(&win_lock);
	k_sem_give(&wake);
	return got >= (int)must;
}

static void reader_thread(void *p1, void *p2, void *p3)
{
	for (;;) {
		const struct clip *want = io_want;

		if (want != sd.clip) {
			file_close();
			if (want != NULL) {
				file_open(want);
			}
			k_sem_give(&wake);
			continue;
		}
		if (sd.clip == NULL || !sd.ready || sd.failed || !file_fill()) {
			k_sem_take(&io_wake, K_MSEC(50));
		}
	}
}
/* Above the worker, below the UI. The reader spends its time waiting for the
 * card and the worker spends its time polling the codec, so this way round
 * the window fills at the card's speed whatever the decoder is doing. Below
 * the worker, a seek on a 14 Mbit/s clip left playback running on an empty
 * window for seconds (16 stalls in a fuzz run).
 */
K_THREAD_DEFINE(player_io, 5120, reader_thread, NULL, NULL, NULL, 2, 0, 0);

/* ---- clock -------------------------------------------------------------- */

/* Sound of clip c from sample frame pos on, up to `frames` frames into dst.
 * Returns how many were delivered: fewer at the end of the clip, or when the
 * reader has not brought that part of a card file in yet (*starved).
 */
static uint32_t pcm_read(const struct clip *c, uint32_t pos, int16_t *dst, uint32_t frames, bool *starved)
{
	uint32_t done = 0;

	*starved = false;
	while (done < frames) {
		uint32_t p = pos + done;
		uint32_t f = p / spf, off = p % spf, len, at;

		if (f >= c->hdr.n_frames) {
			break;
		}
		if (c->mem != NULL) {
			const uint8_t *pcm = chunk_audio(c, f, &len);
			uint32_t n = MIN(frames - done, len / 4 - MIN(off, len / 4));

			if (n == 0) {
				break;
			}
			memcpy(dst + done * 2, pcm + off * 4, n * 4);
			done += n;
		} else {
			if (!file_pcm(c, f, &at, &len)) {
				*starved = true;
				break;
			}
			uint32_t n = MIN(frames - done, len / 4 - MIN(off, len / 4));

			if (n == 0) {
				break;
			}
			if (!win_get(c, at + off * 4, dst + done * 2, n * 4)) {
				*starved = true;
				break;
			}
			done += n;
		}
	}
	return done;
}

/* No step in the output, wherever the position jumps (pause, play, seek,
 * loop): the sound that was playing is not cut, what would have come next
 * fades out over XFADE while the sound at the new position fades in.
 *
 * While a finger is on the slider (player_scrub) the picture follows it and
 * the sound is made of grains: GRAIN of the clip from the frame under the
 * finger, each one cross-faded into the next, and nothing while the finger
 * rests. Before, every UI frame restarted the sound at the finger: a step of
 * up to full scale 60 times a second in a drag, and the same 16 ms over and
 * over under a resting finger.
 *
 * Whatever fades has its samples in memory before it starts. Read from the
 * clip as it went, a voice fell silent or came in at full level whenever the
 * card reader was still on its way to a new position, or had already left
 * the old one: a click per block in a fast drag.
 *
 * All of this state belongs to the audio thread.
 */
#define XFADE_BITS 10
#define XFADE      (1u << XFADE_BITS) /* 21 ms */
#define AHEAD_CAP  (AUDIO_BLOCK_FRAMES + XFADE)

/* a piece of sound on its way out, gain g0 falling to 0 over its n frames */
struct fader {
	int16_t pcm[XFADE * 2];
	uint32_t n, i;
	uint32_t g0;
};
static struct fader fader[2];
static int16_t ahead[AHEAD_CAP * 2]; /* the stream: the sound from a_pos on */
static uint32_t ahead_len;
static uint32_t grain_len, grain_age;
static uint32_t grain_frame;         /* clip frame the last grain was taken from */
static int grain_buf;
static bool grain_on;

static inline int16_t sat16(int32_t v)
{
	return (int16_t)CLAMP(v, -32768, 32767);
}

static inline void add_frame(int16_t *dst, const int16_t *src, uint32_t gain)
{
	dst[0] = sat16(dst[0] + ((src[0] * (int32_t)gain) >> XFADE_BITS));
	dst[1] = sat16(dst[1] + ((src[1] * (int32_t)gain) >> XFADE_BITS));
}

static void fader_mix(struct fader *f, int16_t *dst, uint32_t frames)
{
	if (f->i >= f->n) {
		return; /* done, or never started */
	}

	uint32_t n = MIN(frames, f->n - f->i);

	for (uint32_t k = 0; k < n; k++, f->i++) {
		add_frame(dst + 2 * k, f->pcm + 2 * f->i, f->g0 * (f->n - f->i) / f->n);
	}
}

/* Let up to XFADE frames at src fade out from gain g0. */
static struct fader *fader_start(const int16_t *src, uint32_t n, uint32_t g0)
{
	struct fader *f = &fader[0], *o = &fader[1];

	n = MIN(n, XFADE);
	if (n == 0 || g0 == 0) {
		return NULL;
	}
	/* the free one, or else the one that is nearer its end */
	if (f->i < f->n && (o->i >= o->n || o->g0 * (o->n - o->i) / o->n < f->g0 * (f->n - f->i) / f->n)) {
		f = o;
	}
	memcpy(f->pcm, src, n * 4);
	f->n = n;
	f->i = 0;
	f->g0 = g0;
	return f;
}

static void grain_end(int16_t *dst, uint32_t frames)
{
	struct fader *f = fader_start(grain_pcm[grain_buf] + grain_age * 2, grain_len - grain_age,
				      MIN(grain_age, XFADE));

	grain_on = false;
	if (f != NULL) {
		fader_mix(f, dst, frames); /* created in mid-block: its share of this one */
	}
}

static void grains_mix(const struct clip *c, int16_t *dst, uint32_t frames, bool scrub)
{
	for (uint32_t at = 0; at < frames;) {
		if (!grain_on) {
			uint32_t f = base_frame;
			int b = grain_buf ^ 1;
			bool starved;

			if (!scrub || f == grain_frame) {
				return; /* the finger has not moved since the last grain */
			}
			uint32_t got = pcm_read(c, f * spf, grain_pcm[b], GRAIN, &starved);

			if (starved) {
				return; /* the reader is on its way there: again in the next block */
			}
			grain_frame = f;
			if (got < 2 * XFADE) {
				return; /* the very end of the clip */
			}
			grain_buf = b;
			grain_len = got;
			grain_age = 0;
			grain_on = true;
		}
		const int16_t *src = grain_pcm[grain_buf];
		uint32_t n = MIN(frames - at, grain_len - XFADE - grain_age);

		for (uint32_t k = 0; k < n; k++, grain_age++) {
			add_frame(dst + 2 * (at + k), src + 2 * grain_age, MIN(grain_age, XFADE));
		}
		at += n;
		if (grain_age >= grain_len - XFADE) {
			grain_end(dst + at * 2, frames - at); /* under the next grain, or into silence */
		}
	}
}

/* Audio thread: supply the next block. a_pos keeps advancing past the end of
 * the clip (as silence) so the clock still reaches the last frame. It does
 * not advance over sound the reader has not delivered: the clock waits.
 */
static uint32_t audio_source(int16_t *dst, uint32_t frames, void *ctx)
{
	static const struct clip *clip;  /* the state here is about this clip */
	static bool was_playing, was_scrub;
	static uint32_t expect;          /* a_pos after the last block: anything else is a jump */
	static uint32_t ramp;            /* gain of the stream, 0..XFADE */
	const struct clip *c = use_audio ? cur : NULL;
	uint32_t done = 0;

	ARG_UNUSED(ctx);
	frames = MIN(frames, AUDIO_BLOCK_FRAMES);
	if (c != clip) {
		clip = c;
		was_playing = was_scrub = grain_on = false;
		ahead_len = 0;
		memset(fader, 0, sizeof(fader)); /* i as well: n = 0 with i > 0 is a division by zero in fader_mix */
	}
	if (c == NULL) {
		return 0;
	}

	bool scrub = scrub_on, play = playing && !scrub;
	uint32_t pos = a_pos;

	if (play != was_playing || pos != expect) {
		if (was_playing) {
			fader_start(ahead, ahead_len, ramp);
		}
		ahead_len = 0;
		ramp = 0;
	}
	if (scrub && !was_scrub) {
		grain_frame = base_frame; /* no grain until the finger moves */
	} else if (!scrub && was_scrub && grain_on) {
		grain_end(dst, 0);
	}
	was_playing = play;
	was_scrub = scrub;

	if (play) {
		bool starved = false;

		if (ahead_len < AHEAD_CAP) {
			ahead_len += pcm_read(c, pos + ahead_len, ahead + ahead_len * 2, AHEAD_CAP - ahead_len, &starved);
		}
		done = MIN(frames, ahead_len);
		memcpy(dst, ahead, done * 4);
		for (uint32_t i = 0; i < done && ramp < XFADE; i++, ramp++) {
			dst[2 * i] = (int16_t)((dst[2 * i] * (int32_t)ramp) >> XFADE_BITS);
			dst[2 * i + 1] = (int16_t)((dst[2 * i + 1] * (int32_t)ramp) >> XFADE_BITS);
		}
		ahead_len -= done;
		memmove(ahead, ahead + done * 2, ahead_len * 4);
		a_pos = expect = pos + (done < frames && starved ? done : frames);
	}
	if (scrub || grain_on || fader[0].i < fader[0].n || fader[1].i < fader[1].n) {
		memset(dst + done * 2, 0, (frames - done) * 4);
		fader_mix(&fader[0], dst, frames);
		fader_mix(&fader[1], dst, frames);
		if (grain_pcm[1] != NULL) {
			grains_mix(c, dst, frames, scrub);
		}
		done = frames;
	}
	return done;
}
/* How far ahead of the sound to decode: a frame chosen for the sample at the
 * DAC was decoded and converted (about 14 ms) and then shown at the UI's next
 * present (8 to 25 ms later), so the picture trailed its sound. Recorded by
 * the board itself (tools/record.py, the A/V sync clip at 60 fps), each
 * flash came 23 to 53 ms (mean 33) after its beep. */
#define SHOW_LEAD_MS 30

static uint32_t lead_samples(void)
{
	return use_audio && cur != NULL ? SHOW_LEAD_MS * cur->hdr.audio_rate / 1000 : 0;
}

/* The clip frame whose sound plays `lead` samples from now. */
static uint32_t clock_frame_at(uint32_t lead)
{
	if (!playing || scrub_on) {
		return base_frame; /* paused, or held under the finger on the slider */
	}
	if (use_audio) {
		/* the sample at the DAC right now: what was handed over minus what is still queued */
		int32_t heard = (int32_t)a_pos - (int32_t)audio_queued_frames() + (int32_t)lead;
		uint32_t f = heard > 0 ? (uint32_t)heard / spf : 0;

		return MAX(f, base_frame);
	}
	uint64_t elapsed = (uint32_t)(gfx_now_us() - base_us);

	return base_frame + (uint32_t)(elapsed * cur->hdr.fps_num / ((uint64_t)cur->hdr.fps_den * 1000000));
}

static uint32_t clock_frame(void)
{
	return clock_frame_at(0);
}

static void rebase(uint32_t frame)
{
	base_frame = frame;
	base_us = gfx_now_us();
	a_pos = frame * spf;
	jump_gen++; /* last: the worker reads the position and this as a pair */
}

/* ---- control ------------------------------------------------------------ */

static int open_clip(const struct clip *c)
{
	k_mutex_lock(&codec, K_FOREVER);
	close_req = false;
	scrub_on = false;
	picture_on = true;
	cur = c;
	playing = false;
	decoded_frame = UINT32_MAX;
	last_acquired = UINT32_MAX;
	published = -1;
	memset(&status, 0, sizeof(status));
	status.open = true;
	status.n_frames = cur->hdr.n_frames;
	status.on_card = cur->mem == NULL;
	spf = cur->hdr.audio_rate * cur->hdr.fps_den / cur->hdr.fps_num;
	use_audio = audio_ok() && cur->hdr.audio_rate == AUDIO_RATE && cur->hdr.audio_channels == 2 &&
		    cur->hdr.audio_bits == 16 && spf > 0;
	if (spf == 0) {
		spf = 1;
	}
	rebase(0);
	seek_frame = 0;
	seek_t0 = gfx_now_us();
	seek_pending = true; /* time from open to the first picture */
	need_frame = 0;
	io_want = cur->mem == NULL ? cur : NULL;
	audio_set_source(audio_source, NULL);
	k_mutex_unlock(&codec);
	k_sem_give(&io_wake);
	k_sem_give(&wake);
	return 0;
}

int player_open(int clip)
{
	if (clip < 0 || clip >= n_clips) {
		return -ENOENT;
	}
	return open_clip(&clips[clip]);
}

int player_open_attract(void)
{
	return player_attract_clip() ? open_clip(&lap_clip) : -ENOENT;
}

uint32_t player_position_ms(void)
{
	const struct clip *c = cur;

	if (c == NULL || close_req) {
		return 0;
	}
	if (use_audio && playing && !scrub_on) {
		int32_t heard = (int32_t)a_pos - (int32_t)audio_queued_frames();

		return heard > 0 ? (uint32_t)((uint64_t)heard * 1000 / c->hdr.audio_rate) : 0;
	}
	return (uint32_t)((uint64_t)clock_frame() * 1000 * c->hdr.fps_den / c->hdr.fps_num);
}

/* Does not wait for the worker, which may be in the middle of a frame with
 * the codec locked for another 10 to 25 ms: that wait was a dropped UI frame
 * on every "Back". The worker lets go of the clip at its next turn; until
 * then player_commit() answers -EBUSY.
 */
void player_close(void)
{
	playing = false;
	scrub_on = false;
	close_req = true;
	status.open = false;
	io_want = NULL;
	k_sem_give(&wake);
	k_sem_give(&io_wake); /* the reader closes the file, so that it can be replaced */
}

void player_play(void)
{
	if (cur && !close_req && !playing) {
		rebase(base_frame >= cur->hdr.n_frames - 1 ? 0 : base_frame);
		playing = true;
		k_sem_give(&wake);
	}
}

void player_pause(void)
{
	if (cur && !close_req && playing) {
		/* the frame on screen, which runs SHOW_LEAD_MS ahead of the sound */
		uint32_t f = MIN(clock_frame_at(lead_samples()), cur->hdr.n_frames - 1);

		playing = false;
		rebase(f);
	}
}

void player_seek(uint32_t frame)
{
	if (cur && !close_req) {
		frame = MIN(frame, cur->hdr.n_frames - 1);
		if (frame != seek_frame) {
			/* a held finger asks for the same frame every UI frame: time the first request */
			seek_frame = frame;
			seek_t0 = gfx_now_us();
			seek_pending = true;
		}
		rebase(frame);
		k_sem_give(&wake);
	}
}

void player_scrub(bool on)
{
	if (cur && !close_req && on != scrub_on) {
		if (!on) {
			rebase(base_frame); /* the sound starts again where the picture is */
		}
		scrub_on = on;
		k_sem_give(&wake);
	}
}

void player_set_loop(bool loop)
{
	loop_clip = loop;
}

void player_get_status(struct player_status *st)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*st = status;
	st->playing = playing;
	st->audio_clock = use_audio;
	k_spin_unlock(&lock, key);
}

void player_note_convert(uint32_t us)
{
	status.convert_us = us;
}

void player_show_picture(bool on)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	picture_on = on;
	if (!on) {
		/* nothing stale when it comes back: the view draws black until the
		 * frame due then is decoded, under the screen's fade-in */
		published = -1;
	}
	k_spin_unlock(&lock, key);
	k_sem_give(&wake);
}

const uint8_t *player_acquire(struct jpeg_info *info, uint32_t *frame, bool *fresh)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const uint8_t *p = NULL;

	if (published >= 0) {
		held = published;
		p = bufs[held];
		*info = published_info;
		*frame = published_frame;
		*fresh = published_frame != last_acquired;
		last_acquired = published_frame;
	}
	k_spin_unlock(&lock, key);
	return p;
}

static int still_from_card(const struct clip *c, uint32_t frame, uint8_t *mcu, struct jpeg_info *info)
{
	bool last = frame + 1 >= c->hdr.n_frames;
	int want = last ? 4 : 8;
	uint32_t ent[2], head[2];
	char path[PATH_LEN];
	struct fs_file_t f;
	int ret;

	snprintf(path, sizeof(path), "%s/%s", SD_CLIPS_DIR, c->name);
	fs_file_t_init(&f);
	sdcard_library_lock();
	ret = sdcard_mounted() && !sdcard_gone() ? fs_open(&f, path, FS_O_READ) : -ENODEV;
	if (ret == 0) {
		ret = -EIO;
		if (sdcard_read_at(&f, c->hdr.index_off + 4 * frame, ent, want) == want) {
			uint32_t end = last ? c->hdr.index_off : ent[1];

			if (end <= c->hdr.index_off && end >= ent[0] + 8 && sdcard_read_at(&f, ent[0], head, 8) == 8 &&
			    head[0] <= STAGE_CAP - JPEG_PAD - 4) {
				uint32_t body = ROUND_UP(head[0] + JPEG_PAD, 4);

				if (body <= end - ent[0] - 8 &&
				    sdcard_read_at(&f, ent[0] + 8, sd.stage, body) == (int)body) {
					ret = jpegdec_probe(sd.stage, head[0], info);
					if (ret == 0) {
						ret = jpegdec_decode(sd.stage, head[0], mcu, MCU_CAP, NULL);
					}
				}
			}
		}
		fs_close(&f);
	}
	sdcard_library_unlock();
	return ret;
}

int player_decode_still(int clip, uint32_t frame, uint8_t *mcu, size_t mcu_cap, struct jpeg_info *info)
{
	int ret;

	if (clip < 0 || clip >= n_clips || mcu_cap < MCU_CAP) {
		return -EINVAL;
	}
	const struct clip *c = &clips[clip];

	frame = MIN(frame, c->hdr.n_frames - 1);
	k_mutex_lock(&codec, K_FOREVER);
	if (c->mem != NULL) {
		ret = decode_frame(c, frame, mcu, info, NULL, NULL);
	} else {
		ret = still_from_card(c, frame, mcu, info);
	}
	k_mutex_unlock(&codec);
	return ret;
}

/* ---- the worker --------------------------------------------------------- */

static void player_thread(void *p1, void *p2, void *p3)
{
	uint32_t rate_frame0 = 0, rate_bytes = 0;
	uint32_t done_gen = 0;      /* the jump the last decoded frame belonged to */
	bool starving = false;

	for (;;) {
		k_mutex_lock(&codec, K_FOREVER);
		if (close_req) {
			close_req = false;
			cur = NULL;
			published = -1;
		}
		const struct clip *c = cur;

		if (c == NULL) {
			starving = false;
			k_mutex_unlock(&codec);
			k_sem_take(&wake, K_FOREVER);
			continue;
		}
		/* The frame to show and the jump it belongs to, as a pair: the UI thread
		 * may seek at any point of this loop, and the frame decoded for the old
		 * position must not be taken for the answer to the seek (it was: such
		 * seeks counted as stalls and as dropped frames).
		 */
		uint32_t target, gen, now_f;

		do {
			gen = jump_gen;
			now_f = clock_frame();
			target = clock_frame_at(lead_samples()); /* what will be due when it is on screen */
		} while (gen != jump_gen);

		/* the end of the clip is where its sound ends, not SHOW_LEAD_MS before */
		if (now_f < c->hdr.n_frames && target >= c->hdr.n_frames) {
			target = c->hdr.n_frames - 1;
		}
		if (now_f >= c->hdr.n_frames) {
			if (loop_clip) {
				rebase(0);
				target = 0;
			} else {
				playing = false;
				rebase(c->hdr.n_frames - 1);
				target = c->hdr.n_frames - 1;
			}
			gen = jump_gen;
		}
		need_frame = target;
		if (!picture_on) {
			/* The picture is hidden (the attract lap's clip under the demos):
			 * the clock and the sound go on, no frame is decoded. Each one is
			 * 8 to 9 ms of CPU feeding the codec, 30 a second, above every
			 * thread but the UI's: it starved the recorder's sender, which
			 * then lost pictures in a 60 fps recording of Explorer. */
			decoded_frame = target;
			done_gen = gen;
			k_spinlock_key_t key = k_spin_lock(&lock);

			status.frame = target;
			k_spin_unlock(&lock, key);
			k_mutex_unlock(&codec);
			k_sem_take(&wake, K_MSEC(5));
			continue;
		}
		if (target == decoded_frame) {
			k_mutex_unlock(&codec);
			k_sem_take(&wake, K_MSEC(2));
			continue;
		}

		struct jpeg_info info;
		uint32_t us = 0, bytes = 0, ahead = 0;
		int ret = 0;

		if (c->mem == NULL) {
			ret = stage_frame(c, target, &bytes, &ahead);
			if (ret == -EAGAIN) {
				/* not read yet: hold the clock here and wait for the reader. Only an
				 * underrun in mid-play counts as a stall; after a seek it is expected.
				 */
				if (!starving) {
					starving = true;
					if (playing && gen == done_gen) {
						status.stalls++;
					}
				}
				status.buffering = true;
				if (!use_audio) {
					base_frame = target;
					base_us = gfx_now_us();
				}
				k_mutex_unlock(&codec);
				k_sem_give(&io_wake);
				k_sem_take(&wake, K_MSEC(5));
				continue;
			}
			if (ret == -EIO) {
				playing = false; /* the card is gone: stop where we are */
			}
		}
		starving = false;

		/* a buffer that is neither published nor in the UI's hands */
		k_spinlock_key_t key = k_spin_lock(&lock);
		int b = 0;

		while (b == published || b == held) {
			b++;
		}
		k_spin_unlock(&lock, key);

		if (c->mem != NULL) {
			ret = decode_frame(c, target, bufs[b], &info, &us, &bytes);
		} else if (ret == 0) {
			ret = jpegdec_probe(sd.stage, bytes, &info);
			if (ret == 0) {
				ret = jpegdec_decode(sd.stage, bytes, bufs[b], MCU_CAP, &us);
			}
		}

		bool jump = gen != done_gen; /* the first frame after a seek, play or loop: not a drop */

		done_gen = gen;
		key = k_spin_lock(&lock);
		if (ret == 0) {
			published = b;
			published_frame = target;
			published_info = info;
			if (seek_pending) {
				seek_pending = false;
				status.seek_us = gfx_now_us() - seek_t0;
			}
		}
		if (playing && !jump && decoded_frame != UINT32_MAX && target > decoded_frame + 1) {
			status.dropped += target - decoded_frame - 1;
		}
		status.frame = target;
		status.decode_us = us;
		status.frame_bytes = bytes;
		status.buffering = false;
		status.ahead_kb = ahead / 1024;
		k_spin_unlock(&lock, key);

		/* bit rate over about one second of clip time; a jump starts the second again */
		rate_bytes += bytes;
		uint32_t fps = c->hdr.fps_num / c->hdr.fps_den;

		if (jump) {
			rate_bytes = 0;
			rate_frame0 = target;
		} else if (target < rate_frame0 || target - rate_frame0 >= fps) {
			uint32_t n = target > rate_frame0 ? target - rate_frame0 : 1;

			status.kbps = (uint32_t)((uint64_t)rate_bytes * 8 * c->hdr.fps_num / c->hdr.fps_den / n / 1000);
			rate_bytes = 0;
			rate_frame0 = target;
		}
		decoded_frame = target;
		k_mutex_unlock(&codec);
		k_yield();
	}
}
/* Below the UI thread (priority 0), above the reader and the network services. */
K_THREAD_DEFINE(player, 4096, player_thread, NULL, NULL, NULL, 3, 0, 0);
