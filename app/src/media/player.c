/*
 * Video player. See player.h.
 *
 * Threading: the worker owns the JPEG codec and decodes into one of three MCU
 * buffers in PSRAM. Three, so that it can always find one that is neither the
 * newest published frame nor the one the UI thread is converting.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "platform/audio.h"
#include "platform/gfx.h"
#include "platform/psram.h"
#include "player.h"

LOG_MODULE_REGISTER(player, LOG_LEVEL_INF);

#define FLASH_BASE_ADDR 0x70000000u
#define MEDIA_OFFSET    0x00420000u   /* first byte after the stock Zephyr partitions */
#define MEDIA_MAX_CLIPS 8
#define NBUF            3
#define MCU_CAP         JPEG_MCU_BYTES(800, 480)

struct media_dir {
	char magic[4];
	uint32_t count;
	struct {
		uint32_t offset, size;
	} clip[MEDIA_MAX_CLIPS];
};

static const struct n6v_header *clips[MEDIA_MAX_CLIPS];
static int n_clips;

static uint8_t *bufs[NBUF];
static struct k_spinlock lock;

/* state shared with the UI thread, under `lock` */
static int published = -1;      /* buffer holding the newest decoded frame */
static int held = -1;           /* buffer the UI is using */
static uint32_t published_frame;
static struct jpeg_info published_info;
static uint32_t last_acquired = UINT32_MAX;

static const struct n6v_header *cur;
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
static volatile bool jumped;    /* the position was set (seek, play, loop): the next frame is not a drop */
static struct player_status status;
static K_SEM_DEFINE(wake, 0, 1);
static K_MUTEX_DEFINE(codec);

static const uint32_t *chunk_ptr(const struct n6v_header *h, uint32_t frame)
{
	const uint32_t *index = (const uint32_t *)((const uint8_t *)h + h->index_off);

	return (const uint32_t *)((const uint8_t *)h + index[frame]);
}

int player_init(void)
{
	const struct media_dir *dir = (const struct media_dir *)(FLASH_BASE_ADDR + MEDIA_OFFSET);

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
	if (memcmp(dir->magic, "N6MD", 4) != 0 || dir->count > MEDIA_MAX_CLIPS) {
		LOG_WRN("no media directory in flash at 0x%08x", FLASH_BASE_ADDR + MEDIA_OFFSET);
		return 0;
	}
	for (uint32_t i = 0; i < dir->count; i++) {
		const struct n6v_header *h = (const struct n6v_header *)((const uint8_t *)dir + dir->clip[i].offset);

		if (memcmp(h->magic, "N6V1", 4) != 0 || h->width > 800 || h->height > 480 || h->n_frames == 0 ||
		    h->fps_num == 0 || h->fps_den == 0) {
			LOG_WRN("clip %u: bad header, skipped", i);
			continue;
		}
		clips[n_clips++] = h;
		LOG_INF("clip %d: \"%s\" %ux%u, %u frames at %u/%u fps, %u KB", n_clips - 1, h->title, h->width,
			h->height, h->n_frames, h->fps_num, h->fps_den, h->file_size / 1024);
	}
	return n_clips;
}

int player_clip_count(void)
{
	return n_clips;
}

const struct n6v_header *player_clip(int clip)
{
	return (clip >= 0 && clip < n_clips) ? clips[clip] : NULL;
}

static const uint8_t *chunk_audio(const struct n6v_header *h, uint32_t frame, uint32_t *len)
{
	const uint32_t *chunk = chunk_ptr(h, frame);

	*len = chunk[1];
	return (const uint8_t *)&chunk[2] + ROUND_UP(chunk[0] + JPEG_PAD, 4);
}

/* Audio thread: supply the next block. a_pos keeps advancing past the end of
 * the clip (as silence) so the clock still reaches the last frame.
 */
static uint32_t audio_source(int16_t *dst, uint32_t frames, void *ctx)
{
	const struct n6v_header *h = cur;
	uint32_t done = 0;

	ARG_UNUSED(ctx);
	if (h == NULL || !playing || !use_audio) {
		return 0;
	}
	while (done < frames) {
		uint32_t pos = a_pos + done;
		uint32_t f = pos / spf, off = pos % spf, len;

		if (f >= h->n_frames) {
			break;
		}
		const uint8_t *pcm = chunk_audio(h, f, &len);
		uint32_t n = MIN(frames - done, len / 4 - MIN(off, len / 4));

		if (n == 0) {
			break;
		}
		memcpy(dst + done * 2, pcm + off * 4, n * 4);
		done += n;
	}
	a_pos += frames;
	return done;
}

static uint32_t clock_frame(void)
{
	if (!playing) {
		return base_frame;
	}
	if (use_audio) {
		/* the sample at the DAC right now: what was handed over minus what is still queued */
		int32_t heard = (int32_t)a_pos - (int32_t)audio_queued_frames();
		uint32_t f = heard > 0 ? (uint32_t)heard / spf : 0;

		return MAX(f, base_frame);
	}
	uint64_t elapsed = (uint32_t)(gfx_now_us() - base_us);

	return base_frame + (uint32_t)(elapsed * cur->fps_num / ((uint64_t)cur->fps_den * 1000000));
}

static void rebase(uint32_t frame)
{
	jumped = true;
	base_frame = frame;
	base_us = gfx_now_us();
	a_pos = frame * spf;
}

int player_open(int clip)
{
	if (clip < 0 || clip >= n_clips) {
		return -ENOENT;
	}
	k_mutex_lock(&codec, K_FOREVER);
	cur = clips[clip];
	playing = false;
	decoded_frame = UINT32_MAX;
	last_acquired = UINT32_MAX;
	published = -1;
	memset(&status, 0, sizeof(status));
	status.open = true;
	status.n_frames = cur->n_frames;
	spf = cur->audio_rate * cur->fps_den / cur->fps_num;
	use_audio = audio_ok() && cur->audio_rate == AUDIO_RATE && cur->audio_channels == 2 &&
		    cur->audio_bits == 16 && spf > 0;
	if (spf == 0) {
		spf = 1;
	}
	rebase(0);
	audio_set_source(audio_source, NULL);
	k_mutex_unlock(&codec);
	k_sem_give(&wake);
	return 0;
}

void player_close(void)
{
	k_mutex_lock(&codec, K_FOREVER);
	playing = false;
	cur = NULL;
	status.open = false;
	published = -1;
	k_mutex_unlock(&codec);
}

void player_play(void)
{
	if (cur && !playing) {
		rebase(base_frame >= cur->n_frames - 1 ? 0 : base_frame);
		playing = true;
		k_sem_give(&wake);
	}
}

void player_pause(void)
{
	if (cur && playing) {
		uint32_t f = MIN(clock_frame(), cur->n_frames - 1);

		playing = false;
		rebase(f);
	}
}

void player_seek(uint32_t frame)
{
	if (cur) {
		rebase(MIN(frame, cur->n_frames - 1));
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

static int decode_frame(const struct n6v_header *h, uint32_t frame, uint8_t *mcu, struct jpeg_info *info,
			uint32_t *us, uint32_t *bytes)
{
	const uint32_t *chunk = chunk_ptr(h, frame);
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

int player_decode_still(int clip, uint32_t frame, uint8_t *mcu, size_t mcu_cap, struct jpeg_info *info)
{
	int ret;

	if (clip < 0 || clip >= n_clips || mcu_cap < MCU_CAP) {
		return -EINVAL;
	}
	k_mutex_lock(&codec, K_FOREVER);
	ret = decode_frame(clips[clip], MIN(frame, clips[clip]->n_frames - 1), mcu, info, NULL, NULL);
	k_mutex_unlock(&codec);
	return ret;
}

static void player_thread(void *p1, void *p2, void *p3)
{
	uint32_t rate_frame0 = 0, rate_bytes = 0;

	for (;;) {
		k_mutex_lock(&codec, K_FOREVER);
		if (cur == NULL) {
			k_mutex_unlock(&codec);
			k_sem_take(&wake, K_FOREVER);
			continue;
		}
		uint32_t target = clock_frame();

		if (target >= cur->n_frames) {
			if (loop_clip) {
				rebase(0);
				target = 0;
			} else {
				playing = false;
				rebase(cur->n_frames - 1);
				target = cur->n_frames - 1;
			}
		}
		if (target == decoded_frame) {
			k_mutex_unlock(&codec);
			k_sem_take(&wake, K_MSEC(2));
			continue;
		}

		/* a buffer that is neither published nor in the UI's hands */
		k_spinlock_key_t key = k_spin_lock(&lock);
		int b = 0;

		while (b == published || b == held) {
			b++;
		}
		k_spin_unlock(&lock, key);

		struct jpeg_info info;
		uint32_t us = 0, bytes = 0;
		int ret = decode_frame(cur, target, bufs[b], &info, &us, &bytes);

		bool jump = jumped;

		jumped = false;
		key = k_spin_lock(&lock);
		if (ret == 0) {
			published = b;
			published_frame = target;
			published_info = info;
		}
		if (playing && !jump && decoded_frame != UINT32_MAX && target > decoded_frame + 1) {
			status.dropped += target - decoded_frame - 1;
		}
		status.frame = target;
		status.decode_us = us;
		status.frame_bytes = bytes;
		k_spin_unlock(&lock, key);

		/* bit rate over about one second of clip time */
		rate_bytes += bytes;
		uint32_t fps = cur->fps_num / cur->fps_den;

		if (target < rate_frame0 || target - rate_frame0 >= fps) {
			uint32_t n = target > rate_frame0 ? target - rate_frame0 : 1;

			status.kbps = (uint32_t)((uint64_t)rate_bytes * 8 * cur->fps_num / cur->fps_den / n / 1000);
			rate_bytes = 0;
			rate_frame0 = target;
		}
		decoded_frame = target;
		k_mutex_unlock(&codec);
		k_yield();
	}
}
/* Below the UI thread (priority 0), above the network services. */
K_THREAD_DEFINE(player, 4096, player_thread, NULL, NULL, NULL, 3, 0, 0);
