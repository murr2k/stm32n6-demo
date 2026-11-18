/*
 * Recorder. See recorder.h.
 *
 * Zephyr's VENC driver (CONFIG_VIDEO_ENCODER_H264) brings ST's encoder
 * library and the layer it runs on (EWL: registers, interrupt, memory pool
 * in PSRAM), but takes only NV12 or RGB565 frames. The library itself reads
 * 32-bit RGB, the frame buffer's own layout, so this calls the library
 * directly and leaves the driver's video API unused: no copy and no
 * conversion of 1.5 MB a picture.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include <ewl.h>
#include <h264encapi.h>

#include "media/recorder.h"
#include "platform/audio.h"
#include "platform/gfx.h"
#include "platform/net.h"
#include "platform/psram.h"
#include "ui/shell.h"

LOG_MODULE_REGISTER(rec, LOG_LEVEL_INF);

#define OUT_N     4
#define OUT_BYTES (512 * 1024) /* an IDR picture of the busiest screen is well under this */
/* Quality: a fixed QP, no rate control. User interface pictures are sharp
 * edges and small type; 20 keeps one-pixel lines clean. P pictures of what
 * does not move cost next to nothing. */
#define REC_QP    20
#define REC_GOP   60 /* an IDR picture every 2 s: a player can start anywhere */

struct slot {
	struct rec_hdr h;
	uint8_t *buf;
};

static struct slot slots[OUT_N];
K_MSGQ_DEFINE(free_q, sizeof(uint8_t), OUT_N, 1);
K_MSGQ_DEFINE(full_q, sizeof(uint8_t), OUT_N, 1);
static K_SEM_DEFINE(start_sem, 0, 1);
static K_SEM_DEFINE(stopped_sem, 0, 1);
static volatile bool want;
static volatile uint32_t every = 2; /* refreshes per picture: 2 is 30 fps, 1 is 60 */
static H264EncInst enc;
static struct k_spinlock lock;
static struct rec_status st; /* under lock */

/*
 * The cache, around the encoder. The library writes the stream's headers
 * (SPS, PPS) with the CPU, into the output buffer, so they sit in the data
 * cache; then it starts the hardware, which writes the picture data after
 * them into memory. Cleaning only before the call, or invalidating after it,
 * loses the headers (the stream did not decode: "non-existing PPS 0"). The
 * moment that works is the hardware's start, in the driver's EWLEnableHW():
 * the link wraps it (app/CMakeLists.txt, -Wl,--wrap=EWLEnableHW), and the
 * wrapper writes the whole data cache back and drops it (set and way,
 * tens of us) before the hardware runs. What the CPU wrote is then in
 * memory, and no stale copy of the output buffer is left to read; what the
 * library writes after the hardware is done goes through the cache over
 * fresh lines. The frame buffer is clean since its present.
 */
void __real_EWLEnableHW(const void *instance, uint32_t offset, uint32_t val);

void __wrap_EWLEnableHW(const void *instance, uint32_t offset, uint32_t val)
{
	sys_cache_data_flush_and_invd_all();
	__real_EWLEnableHW(instance, offset, val);
}

/*
 * The driver gives the hardware 100 ms per picture. On the Camera screen,
 * with the NPU's runs taking the PSRAM, a picture was declared timed out
 * twice in a tour, and the hardware, starved rather than stuck, finished
 * later: the recovery had freed its memory by then, and its late interrupt
 * would release the next picture's wait early. The pool's heap was found
 * corrupted (MPU fault in EWLMallocLinear). So the wait gets up to 500 ms
 * (also wrapped at link time), and an error resets the encoder through its
 * reset line before anything of its memory is released (enc_reset()).
 */
i32 __real_EWLWaitHwRdy(const void *instance, uint32_t *slices_ready);

static volatile uint32_t long_waits;

i32 __wrap_EWLWaitHwRdy(const void *instance, uint32_t *slices_ready)
{
	i32 r = EWL_HW_WAIT_TIMEOUT;

	for (int k = 0; k < 5 && r == EWL_HW_WAIT_TIMEOUT; k++) {
		r = __real_EWLWaitHwRdy(instance, slices_ready);
		long_waits += r == EWL_HW_WAIT_TIMEOUT;
	}
	return r;
}

static const struct reset_dt_spec venc_reset = RESET_DT_SPEC_GET(DT_NODELABEL(venc));

/*
 * The encoder's linear memory (reference frames, its internal buffers). The
 * driver's EWLMallocLinear() records each allocation at chunks[total_chunks++]
 * in an array of 32, and EWLFreeLinear() frees the memory but never takes
 * the entry out; nothing resets the count. Every H264EncInit() allocates
 * several, so after a few starts of the encoder (a new recording, a restart
 * after an error) the count runs past the array and corrupts the driver's
 * state and the pool's heap: an MPU fault in EWLMallocLinear, after 3 to 4
 * restarts on the Camera screen. These take the memory from the same pool
 * through the driver's EWLmalloc()/EWLfree(), which keep no table. The
 * library's calls are routed here at link time; the driver's own calls
 * (none of ours) are not.
 */
i32 __wrap_EWLMallocLinear(const void *instance, uint32_t size, EWLLinearMem_t *info)
{
	uint32_t n = ROUND_UP(size, 8);
	void *p = EWLmalloc(n);

	ARG_UNUSED(instance);
	if (p == NULL) {
		return EWL_ERROR;
	}
	info->virtualAddress = p;
	info->busAddress = (ptr_t)p; /* no MMU: the bus sees the same address */
	info->size = n;
	return EWL_OK;
}

void __wrap_EWLFreeLinear(const void *instance, EWLLinearMem_t *info)
{
	ARG_UNUSED(instance);
	EWLfree(info->virtualAddress);
	info->virtualAddress = NULL;
	info->busAddress = 0;
	info->size = 0;
}

i32 __wrap_EWLMallocRefFrm(const void *instance, uint32_t size, EWLLinearMem_t *info)
{
	return __wrap_EWLMallocLinear(instance, size, info);
}

void __wrap_EWLFreeRefFrm(const void *instance, EWLLinearMem_t *info)
{
	__wrap_EWLFreeLinear(instance, info);
}

static void fail(int code)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	st.errors++;
	st.last_error = code;
	k_spin_unlock(&lock, key);
}

/* Set the encoder up and write the SPS and PPS into s. */
static int enc_open(struct slot *s)
{
	H264EncConfig cfg = {0};
	H264EncPreProcessingCfg pre = {0};
	H264EncCodingCtrl cod = {0};
	H264EncRateCtrl rc = {0};
	H264EncIn in = {0};
	H264EncOut out = {0};
	H264EncRet r;

	cfg.streamType = H264ENC_BYTE_STREAM;
	cfg.viewMode = H264ENC_BASE_VIEW_SINGLE_BUFFER;
	cfg.level = H264ENC_LEVEL_4;
	cfg.width = GFX_W;
	cfg.height = GFX_H;
	cfg.frameRateNum = 60;      /* the time scale: one tick per panel refresh */
	cfg.frameRateDenom = every; /* a picture every `every` refreshes */
	cfg.refFrameAmount = 1;
	r = H264EncInit(&cfg, &enc);
	if (r != H264ENC_OK) {
		LOG_ERR("H264EncInit %d", r);
		return r;
	}
	r = H264EncGetPreProcessing(enc, &pre);
	if (r == H264ENC_OK) {
		pre.origWidth = GFX_W;
		pre.origHeight = GFX_H;
		pre.inputType = H264ENC_RGB888; /* 0x00RRGGBB words: the frame buffer's XRGB8888 */
		pre.colorConversion.type = H264ENC_RGBTOYUV_BT601;
		r = H264EncSetPreProcessing(enc, &pre);
	}
	if (r == H264ENC_OK) {
		r = H264EncGetCodingCtrl(enc, &cod);
	}
	if (r == H264ENC_OK) {
		cod.idrHeader = 1; /* SPS and PPS before every IDR picture */
		r = H264EncSetCodingCtrl(enc, &cod);
	}
	if (r == H264ENC_OK) {
		r = H264EncGetRateCtrl(enc, &rc);
	}
	if (r == H264ENC_OK) {
		rc.pictureRc = 0;
		rc.mbRc = 0;
		rc.pictureSkip = 0;
		rc.hrd = 0;
		rc.qpHdr = REC_QP;
		rc.qpMin = REC_QP;
		rc.qpMax = REC_QP;
		rc.fixedIntraQp = REC_QP;
		rc.gopLen = REC_GOP;
		r = H264EncSetRateCtrl(enc, &rc);
	}
	if (r == H264ENC_OK) {
		in.pOutBuf = (u32 *)s->buf;
		in.busOutBuf = (size_t)s->buf;
		in.outBufSize = OUT_BYTES;
		r = H264EncStrmStart(enc, &in, &out);
	}
	if (r != H264ENC_OK) {
		LOG_ERR("encoder setup %d", r);
		H264EncRelease(enc);
		enc = NULL;
		return r;
	}
	memset(&s->h, 0, sizeof(s->h)); /* the headers are the CPU's own writes: read through the cache */
	s->h.kind = REC_HEADERS;
	s->h.size = out.streamSize;
	return 0;
}

static void enc_close(void)
{
	if (enc != NULL) {
		H264EncIn in = {0};
		H264EncOut out = {0};
		uint8_t i;

		/* the end-of-stream bytes go to a free buffer and nowhere else */
		if (k_msgq_get(&free_q, &i, K_NO_WAIT) == 0) {
			in.pOutBuf = (u32 *)slots[i].buf;
			in.busOutBuf = (size_t)slots[i].buf;
			in.outBufSize = OUT_BYTES;
			H264EncStrmEnd(enc, &in, &out);
			k_msgq_put(&free_q, &i, K_NO_WAIT);
		}
		H264EncRelease(enc);
		enc = NULL;
	}
}

/* After an error: stop whatever the hardware is still doing, then let the
 * library go. No end-of-stream: that would start the hardware again. */
static void enc_reset(void)
{
	reset_line_toggle_dt(&venc_reset);
	if (enc != NULL) {
		H264EncRelease(enc);
		enc = NULL;
	}
}

/* Cooperative: it starts the encoder at the beginning of a refresh without
 * waiting for the UI thread, then sleeps while the hardware works. */
static void enc_thread(void *a, void *b, void *c)
{
	for (;;) {
		uint8_t i;

		k_sem_take(&start_sem, K_FOREVER);
		if (k_msgq_get(&free_q, &i, K_MSEC(100)) != 0 || enc_open(&slots[i]) != 0) {
			fail(-EIO);
			want = false;
			k_sem_give(&stopped_sem);
			continue;
		}
		k_msgq_put(&full_q, &i, K_NO_WAIT);

		k_spinlock_key_t key = k_spin_lock(&lock);

		st.recording = true;
		st.pictures = st.missed = st.bytes = 0;
		st.encode_worst_us = 0;
		k_spin_unlock(&lock, key);

		uint32_t period = gfx_stats()->period_us;
		uint32_t t_prev = gfx_sleep_to_active(), refresh = 0, last = 0, n = 0, gop = 0, missed = 0, held = 0;
		bool first = true;

		while (want) {
			uint32_t t = gfx_sleep_to_active();
			/* the DAC frame playing as this refresh's picture appears */
			int32_t apos = (int32_t)(audio_tap_total() - audio_queued_frames());

			/* refreshes since the last wake, rounded each time: no drift */
			refresh += (t - t_prev + period / 2) / period;
			t_prev = t;
			if (!first && refresh - last < every) {
				continue;
			}
			if (!first && refresh - last > every) {
				missed += (refresh - last) / every - 1; /* overslept a slot */
			}
			if (gfx_single_buffered()) {
				/* The Camera screen: the display draws into the buffer it
				 * shows while the NPU has the other, so the encoder (12 to
				 * 17 ms there, its bus starved by the NPU's PSRAM traffic:
				 * 209 hardware timeouts in 2 minutes) made the display wait
				 * for it, up to 18 ms, and frames dropped. Not recorded for
				 * now: the receiver holds the picture before. */
				held++;
				last = refresh;
				continue;
			}
			if (k_msgq_get(&free_q, &i, K_NO_WAIT) != 0) {
				missed++; /* the network is behind */
				last = refresh;
				continue;
			}
			struct slot *s = &slots[i];
			const uint8_t *buf = gfx_front();
			H264EncIn in = {0};
			H264EncOut out = {0};
			bool idr = gop % REC_GOP == 0;

			in.busLuma = (ptr_t)buf; /* RGB: the whole picture */
			in.timeIncrement = first ? 0 : refresh - last;
			in.pOutBuf = (u32 *)s->buf;
			in.busOutBuf = (size_t)s->buf;
			in.outBufSize = OUT_BYTES;
			in.codingType = idr ? H264ENC_INTRA_FRAME : H264ENC_PREDICTED_FRAME;
			in.ipf = H264ENC_REFERENCE_AND_REFRESH;
			in.ltrf = H264ENC_REFERENCE;

			gfx_reader_begin(buf);

			uint32_t c0 = k_cycle_get_32();
			H264EncRet r = H264EncStrmEncode(enc, &in, &out, NULL, NULL, NULL);
			uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);

			gfx_reader_end();
			if (r != H264ENC_FRAME_READY || out.streamSize == 0) {
				/* Reset the hardware, then a new stream (SPS, PPS) from an IDR
				 * picture, as the Zephyr driver restarts after an error. */
				LOG_WRN("encoder error %d at picture %u: starting again", r, n);
				fail(r);
				enc_reset();
				if (enc_open(s) != 0) {
					k_msgq_put(&free_q, &i, K_NO_WAIT);
					want = false;
					break;
				}
				k_msgq_put(&full_q, &i, K_NO_WAIT);
				gop = 0;
				last = refresh;
				continue;
			}
			s->h = (struct rec_hdr){
				.kind = REC_PICTURE,
				.coding = idr ? 0 : 1,
				.screen = (uint8_t)shell_current(),
				.flags = gfx_single_buffered() ? REC_FLAG_SINGLE : 0,
				.refresh = refresh,
				.t_us = t,
				.encode_us = us,
				.audio_pos = apos > 0 ? (uint32_t)apos : 0,
				.size = out.streamSize,
				.missed = missed,
			};
			k_msgq_put(&full_q, &i, K_NO_WAIT);
			first = false;
			last = refresh;
			n++;
			gop++;

			key = k_spin_lock(&lock);
			st.pictures = n;
			st.missed = missed;
			st.held = held;
			st.encode_us = us;
			st.encode_worst_us = MAX(st.encode_worst_us, us);
			if (idr) {
				st.idr_bytes = out.streamSize;
			} else {
				st.p_bytes = out.streamSize;
			}
			k_spin_unlock(&lock, key);
		}
		enc_close();
		key = k_spin_lock(&lock);
		st.recording = false;
		k_spin_unlock(&lock, key);
		k_sem_give(&stopped_sem);
	}
}
K_THREAD_DEFINE(rec_enc, 4096, enc_thread, NULL, NULL, NULL, K_PRIO_COOP(4), 0, 0);

static bool send_all(int s, const void *p, size_t n)
{
	const uint8_t *b = p;

	while (n > 0) {
		ssize_t k = zsock_send(s, b, MIN(n, (size_t)16384), 0);

		if (k <= 0) {
			return false;
		}
		b += k;
		n -= (size_t)k;
	}
	return true;
}

/* The sound since *apos, as REC_AUDIO messages straight from the tap's
 * ring. Behind by more than the ring holds, it skips ahead; the receiver
 * sees the jump in audio_pos and fills it with silence. */
static bool send_audio(int s, uint32_t *apos, uint32_t *bytes)
{
	for (int k = 0; k < 8; k++) {
		const int16_t *p;
		uint32_t total = audio_tap_total();

		if (total - *apos > AUDIO_TAP_FRAMES - 8192) {
			*apos = total - 4096;
		}
		uint32_t n = MIN(audio_tap_span(*apos, &p), 4096u);

		if (n == 0) {
			return true;
		}
		struct rec_hdr h = {
			.magic = 0x5652364e,
			.kind = REC_AUDIO,
			.audio_pos = *apos,
			.size = n * 4,
		};

		if (!send_all(s, &h, sizeof(h)) || !send_all(s, p, n * 4)) {
			return false;
		}
		*apos += n;
		*bytes += sizeof(h) + n * 4;
	}
	return true;
}

/* One client: connected, it records; closed, it stops. */
static void net_thread(void *a, void *b, void *c)
{
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(REC_PORT)};
	int srv;

	for (int k = 0; k < OUT_N; k++) {
		slots[k].buf = psram_alloc(OUT_BYTES, 64);
		if (slots[k].buf == NULL) {
			LOG_ERR("no PSRAM for the recorder");
			return;
		}
	}
	net_wait_ip();
	srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (srv < 0 || zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || zsock_listen(srv, 1) < 0) {
		LOG_ERR("recorder: socket setup failed (%d)", errno);
		return;
	}
	LOG_INF("recorder on tcp/%d", REC_PORT);
	for (;;) {
		int s = zsock_accept(srv, NULL, NULL);
		uint8_t i;

		if (s < 0) {
			continue;
		}
		/* a receiver that stops reading must not hold the recorder for ever */
		struct zsock_timeval stall = {.tv_sec = 5};

		zsock_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &stall, sizeof(stall));

		/* an optional first line from the client: "fps 60" or "fps 30" (the default) */
		struct zsock_timeval wait = {.tv_usec = 300000};
		char line[24] = "";
		ssize_t got;

		zsock_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
		got = zsock_recv(s, line, sizeof(line) - 1, 0);
		every = got > 0 && strncmp(line, "fps 60", 6) == 0 ? 1 : 2;

		k_msgq_purge(&free_q);
		k_msgq_purge(&full_q);
		for (uint8_t k = 0; k < OUT_N; k++) {
			k_msgq_put(&free_q, &k, K_NO_WAIT);
		}
		k_sem_reset(&stopped_sem);
		audio_tap(true); /* the sound from now on, frame 0 */
		want = true;
		k_sem_give(&start_sem);

		uint32_t bytes = 0, apos = 0;

		while (want) {
			char c;

			if (!send_audio(s, &apos, &bytes)) {
				want = false;
				break;
			}
			if (k_msgq_get(&full_q, &i, K_MSEC(20)) != 0) {
				/* no picture: has the client gone? */
				if (zsock_recv(s, &c, 1, ZSOCK_MSG_DONTWAIT) == 0) {
					want = false;
				}
				continue;
			}
			struct slot *sl = &slots[i];

			sl->h.magic = 0x5652364e; /* "N6RV" */
			if (!send_all(s, &sl->h, sizeof(sl->h)) || !send_all(s, sl->buf, sl->h.size)) {
				want = false;
			}
			bytes += sizeof(sl->h) + sl->h.size;
			k_msgq_put(&free_q, &i, K_NO_WAIT);

			k_spinlock_key_t key = k_spin_lock(&lock);

			st.bytes = bytes;
			k_spin_unlock(&lock, key);
		}
		k_sem_take(&stopped_sem, K_SECONDS(2));
		audio_tap(false);
		zsock_close(s);
		LOG_INF("recording ended: %u bytes", bytes);
	}
}
K_THREAD_DEFINE(rec_net, 3072, net_thread, NULL, NULL, NULL, 10, 0, 0);

void recorder_get(struct rec_status *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = st;
	k_spin_unlock(&lock, key);
	out->long_waits = long_waits;
}
