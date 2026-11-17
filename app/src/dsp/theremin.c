/*
 * Theremin. See theremin.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <arm_math.h>

#include "platform/audio.h"
#include "platform/gfx.h"
#include "platform/psram.h"
#include "platform/touch.h"
#include "sig.h"
#include "theremin.h"

#define RING_N   8192   /* analysis ring, samples, power of two */
#define CHUNK    32     /* pitch is set per chunk (0.67 ms) and interpolated within */
#define TWO_PI_F 6.28318530718f
#define SILENT   1e-5f  /* amplitude (-100 dBFS) below which a voice is idle */

struct voice {
	float phase;      /* cycles */
	float inc;        /* cycles a sample */
	float lf, lf_t;   /* log2 of the pitch: now and target */
	float amp, amp_t; /* amplitude of the fundamental, full scale 1: now and target */
	float vib;        /* vibrato depth, a fraction of the pitch */
	float vib_ph;
	bool held;
};

struct target {
	bool on;
	float hz, db, vib;
	uint32_t t_us; /* the touch report it came from (0 if none) */
};

static struct voice vo[THM_VOICES];
static struct k_spinlock lock;
static struct {
	int x1, y1, x2, y2, px0, px1;
} field;                                /* under lock */
static struct target remote_t[THM_VOICES]; /* under lock */
static struct thm_status pub;            /* under lock */
static volatile bool enabled, closing, idle, ghost_on, remote_on, snap_on, fresh;
static volatile bool sound_on = true;
static volatile int wave = THM_THEREMIN;
static volatile float rate = AUDIO_RATE;
static uint32_t ghost_n;                 /* samples since the ghost began */
static int16_t *ring;
static volatile uint32_t ring_w;
static uint32_t n_blocks, lat_last, lat_worst, lat_n;
static float acc[AUDIO_BLOCK_FRAMES];

/* ---- the ghost: a flying saucer -----------------------------------------
 * Two voices on keyframes, log pitch and level eased between them, with the
 * wide slow vibrato of 1950s science fiction: an approach that swoops up, a
 * hover that wobbles, a whistle, a second voice beating against the first,
 * a zip and a dive to land. 13 s, then again.
 */
struct kf {
	float t, hz, db, vib;
};

#define GHOST_LOOP 13.0f

static const struct kf ghost0[] = {
	{0.0f, 240, -90, 0.02f},  {0.6f, 300, -17, 0.02f},  {2.4f, 1250, -14, 0.025f}, {3.6f, 980, -16, 0.03f},
	{4.6f, 1700, -15, 0.02f}, {5.4f, 1500, -15, 0.035f}, {7.2f, 520, -20, 0.02f},  {8.2f, 760, -17, 0.03f},
	{9.0f, 2200, -16, 0.015f}, {10.8f, 180, -24, 0.03f}, {11.8f, 160, -90, 0.02f}, {13.0f, 240, -90, 0.02f},
};
static const struct kf ghost1[] = {
	{0.0f, 1300, -90, 0.02f},  {2.2f, 1300, -90, 0.02f}, {3.0f, 1030, -23, 0.03f}, {4.6f, 1760, -23, 0.02f},
	{6.8f, 560, -27, 0.03f},   {7.4f, 520, -90, 0.02f},  {13.0f, 520, -90, 0.02f},
};

static void ghost_at(const struct kf *k, int n, float t, struct target *g)
{
	int i = 0;

	while (i < n - 2 && t >= k[i + 1].t) {
		i++;
	}
	float u = CLAMP((t - k[i].t) / (k[i + 1].t - k[i].t), 0.0f, 1.0f);

	u = u * u * (3.0f - 2.0f * u); /* eased: no corner at a keyframe */
	g->hz = exp2f(log2f(k[i].hz) + u * (log2f(k[i + 1].hz) - log2f(k[i].hz)));
	g->db = k[i].db + u * (k[i + 1].db - k[i].db);
	g->vib = k[i].vib + u * (k[i + 1].vib - k[i].vib);
	g->on = g->db > -70.0f;
	g->t_us = 0;
}

/* ---- inputs ---------------------------------------------------------------- */

float thm_u_db(float u)
{
	if (u <= 0.0f) {
		return SIG_DB_MIN;
	}
	float db = THM_TOP_DB + 10.0f * log2f(MIN(u, 1.0f));

	return MAX(db, SIG_DB_MIN);
}

float thm_db_u(float db)
{
	return db >= THM_TOP_DB ? 1.0f : db <= SIG_DB_MIN ? 0.0f : exp2f((db - THM_TOP_DB) * 0.1f);
}

/* A touch slot, from the audio thread's view. A finger plays from where it
 * comes down on the field until it lifts, wherever it goes meanwhile: off the
 * field's top or bottom it keeps its last level, past either end the pitch
 * stops at that end. A finger that comes down anywhere else (a button, an edge
 * swipe, a finger still down from the swipe that opened the screen) never
 * plays. */
enum { SLOT_UP, SLOT_PLAYS, SLOT_IGNORED };
static uint8_t slot[THM_VOICES];
static float slot_db[THM_VOICES];

static void from_touch(struct target *t)
{
	struct touch_state ts;
	k_spinlock_key_t key = k_spin_lock(&lock);
	int x1 = field.x1, y1 = field.y1, y2 = field.y2, px0 = field.px0, px1 = field.px1;
	int h = y2 - y1 + 1;

	k_spin_unlock(&lock, key);
	touch_get(&ts);
	if (fresh) {
		/* just enabled: whatever is down already came down before */
		for (int v = 0; v < MIN(THM_VOICES, TOUCH_MAX_POINTS); v++) {
			slot[v] = ts.pt[v].down ? SLOT_IGNORED : SLOT_UP;
		}
		fresh = false;
	}
	for (int v = 0; v < MIN(THM_VOICES, TOUCH_MAX_POINTS); v++) {
		const struct touch_point *p = &ts.pt[v];
		bool in_y = p->y >= y1 && p->y <= y2;

		if (!p->down || h <= 3) {
			slot[v] = SLOT_UP;
			continue;
		}
		if (slot[v] == SLOT_UP) {
			slot[v] = in_y && p->x >= px0 && p->x <= px1 ? SLOT_PLAYS : SLOT_IGNORED;
		}
		if (slot[v] != SLOT_PLAYS) {
			continue;
		}
		/* the inverse of the spectrum's axes: its column, its level (siglab.c
		 * db_to_y: the floor two rows above the bottom, the top one below) */
		if (in_y) {
			slot_db[v] = thm_u_db((float)(y2 - 1 - p->y) / (float)(h - 3));
		}
		t[v].on = true;
		t[v].hz = sig_colf_hz((float)(CLAMP((int)p->x, px0, px1) - x1));
		t[v].db = slot_db[v];
		t[v].vib = wave == THM_THEREMIN ? 0.005f : 0.0f;
		t[v].t_us = p->t_us;
	}
}

static void get_targets(struct target *t)
{
	bool touch = false;

	memset(t, 0, sizeof(*t) * THM_VOICES);
	if (remote_on) {
		k_spinlock_key_t key = k_spin_lock(&lock);

		memcpy(t, remote_t, sizeof(remote_t));
		k_spin_unlock(&lock, key);
	} else if (ghost_on) {
		float s = fmodf((float)ghost_n / rate, GHOST_LOOP);

		ghost_at(ghost0, ARRAY_SIZE(ghost0), s, &t[0]);
		ghost_at(ghost1, ARRAY_SIZE(ghost1), s, &t[1]);
	} else if (enabled && !closing) {
		from_touch(t);
		touch = true;
	}
	if (!touch) {
		/* when the touch takes over from the ghost, the finger that ended attract
		 * mode is down already and plays (if it is on the field) */
		memset(slot, SLOT_UP, sizeof(slot));
		fresh = false;
	}
	if (snap_on) {
		for (int v = 0; v < THM_VOICES; v++) {
			if (t[v].on) {
				t[v].hz = 440.0f * exp2f(roundf(12.0f * log2f(t[v].hz / 440.0f)) / 12.0f);
			}
		}
	}
}

/* ---- synthesis --------------------------------------------------------------- */

static inline float polyblep(float t, float dt)
{
	if (t < dt) {
		t /= dt;
		return t + t - t * t - 1.0f;
	}
	if (t > 1.0f - dt) {
		t = (t - 1.0f) / dt;
		return t * t + t + t + 1.0f;
	}
	return 0.0f;
}

/* One sample of the waveform, scaled so its fundamental has amplitude 1: the
 * level a finger asks for is the level of the peak under it. g2 and g3 drop
 * the theremin's overtones that would pass Nyquist. */
static inline float shape(int w, float ph, float inc, float g2, float g3)
{
	switch (w) {
	case THM_SINE:
		return arm_sin_f32(TWO_PI_F * ph);
	case THM_THEREMIN: {
		float p2 = 2.0f * ph, p3 = 3.0f * ph;

		p2 -= (float)(int)p2;
		p3 -= (float)(int)p3;
		return arm_sin_f32(TWO_PI_F * ph) + g2 * 0.18f * arm_sin_f32(TWO_PI_F * p2) +
		       g3 * 0.07f * arm_sin_f32(TWO_PI_F * p3);
	}
	case THM_TRIANGLE:
		return (4.0f * fabsf(ph - 0.5f) - 1.0f) * 1.2337006f; /* fundamental 8 / pi^2 */
	default:
		return (2.0f * ph - 1.0f - polyblep(ph, inc)) * 1.5707963f; /* sawtooth, fundamental 2 / pi */
	}
}

static uint32_t theremin_audio(int16_t *dst, uint32_t frames, void *ctx)
{
	ARG_UNUSED(ctx);
	struct target t[THM_VOICES];
	uint32_t now = gfx_now_us(), queued = audio_queued_frames(), c0 = k_cycle_get_32();
	float fs = rate;
	int n = (int)MIN(frames, (uint32_t)AUDIO_BLOCK_FRAMES), w = ghost_on ? THM_THEREMIN : wave;
	/* time constants: glide 8 ms a chunk; level: attack 4 ms, moves 6 ms,
	 * release 60 ms (about 0.4 s to silence), 8 ms when giving the output back */
	float k_glide = 1.0f - expf(-(float)CHUNK / (0.008f * fs));
	float k_att = 1.0f - expf(-1.0f / (0.004f * fs)), k_lvl = 1.0f - expf(-1.0f / (0.006f * fs));
	float k_rel = 1.0f - expf(-1.0f / ((closing ? 0.008f : 0.060f) * fs));
	float vib_rate = ghost_on ? 6.5f : 5.5f;
	bool quiet = true;

	get_targets(t);
	memset(acc, 0, sizeof(acc));
	for (int v = 0; v < THM_VOICES; v++) {
		struct voice *p = &vo[v];
		const struct target *g = &t[v];
		bool on = g->on && !closing;

		if (on) {
			float lf = log2f(MAX(g->hz, 10.0f));

			if (!p->held && p->amp < SILENT) {
				p->lf = lf; /* a new note starts at its pitch, no glide from the last one */
				p->inc = exp2f(lf) / fs;
				if (g->t_us != 0) {
					/* the panel's report, to the block's first sample at the DAC */
					uint32_t ahead = queued > AUDIO_BLOCK_FRAMES ? queued - AUDIO_BLOCK_FRAMES : 0;
					uint32_t lat = (now - g->t_us) + (uint32_t)((float)ahead * 1e6f / fs);

					lat_last = lat;
					lat_worst = MAX(lat_worst, lat);
					lat_n++;
				}
			}
			p->lf_t = lf;
			p->amp_t = powf(10.0f, g->db / 20.0f);
			p->vib = g->vib;
		} else {
			p->amp_t = 0.0f;
		}
		p->held = on;
		if (p->amp < SILENT && p->amp_t == 0.0f) {
			p->amp = 0.0f;
			continue;
		}
		quiet = false;
		for (int c = 0; c < n; c += CHUNK) {
			p->lf += k_glide * (p->lf_t - p->lf);
			float f = exp2f(p->lf);

			if (p->vib > 0.0f) {
				p->vib_ph += vib_rate * CHUNK / fs;
				p->vib_ph -= (float)(int)p->vib_ph;
				f *= 1.0f + p->vib * arm_sin_f32(TWO_PI_F * p->vib_ph);
			}
			float inc_t = MIN(f / fs, 0.45f), d = (inc_t - p->inc) / CHUNK;
			float g2 = 2.0f * f < 20000.0f ? 1.0f : 0.0f, g3 = 3.0f * f < 20000.0f ? 1.0f : 0.0f;

			for (int i = c; i < MIN(c + CHUNK, n); i++) {
				p->inc += d;
				p->phase += p->inc;
				if (p->phase >= 1.0f) {
					p->phase -= 1.0f;
				}
				float k = p->amp_t > p->amp ? k_att : p->held ? k_lvl : k_rel;

				p->amp += k * (p->amp_t - p->amp);
				acc[i] += p->amp * shape(w, p->phase, p->inc, g2, g3);
			}
		}
	}
	/* a soft limiter from -4.4 dBFS, ceiling -0.9 dBFS: five loud fingers bend,
	 * they do not clip. Then the SOUND switch, a 10 ms fade: the analysis gets
	 * what was played either way. */
	static float out_gain = 1.0f;
	float gain_t = sound_on ? 1.0f : 0.0f, k_gain = 1.0f - expf(-1.0f / (0.010f * fs));

	for (int i = 0; i < n; i++) {
		float x = acc[i], ax = fabsf(x);

		if (ax > 0.6f) {
			x = copysignf(0.6f + 0.3f * tanhf((ax - 0.6f) / 0.3f), x);
		}
		int16_t s = (int16_t)(x * 32767.0f);

		out_gain += k_gain * (gain_t - out_gain);
		dst[2 * i] = dst[2 * i + 1] = out_gain == 1.0f ? s : (int16_t)(x * out_gain * 32767.0f);
		if (ring) {
			ring[(ring_w + i) & (RING_N - 1)] = s;
		}
	}
	if (fabsf(gain_t - out_gain) < 1e-4f) {
		out_gain = gain_t; /* -80 dB from the end: exactly there */
	}
	ring_w += n;
	if (ghost_on) {
		ghost_n += n;
	}
	idle = quiet;
	n_blocks++;

	uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);
	k_spinlock_key_t key = k_spin_lock(&lock);

	pub.synth_us = us;
	pub.synth_worst_us = MAX(pub.synth_worst_us, us);

	for (int v = 0; v < THM_VOICES; v++) {
		pub.v[v].on = vo[v].amp >= SILENT;
		pub.v[v].held = vo[v].held;
		pub.v[v].hz = exp2f(vo[v].lf);
		pub.v[v].db = 20.0f * log10f(MAX(vo[v].amp, SILENT));
	}
	pub.blocks = n_blocks;
	pub.lat_last_us = lat_last;
	pub.lat_worst_us = lat_worst;
	pub.lat_n = lat_n;
	k_spin_unlock(&lock, key);
	return (uint32_t)n;
}

/* ---- control ----------------------------------------------------------------- */

void theremin_init(void)
{
	ring = psram_alloc(RING_N * sizeof(int16_t), 32);
	if (ring) {
		memset(ring, 0, RING_N * sizeof(int16_t));
	}
}

void theremin_enable(bool on)
{
	if (on) {
		if (!enabled || closing) {
			fresh = true; /* a finger down now came down before: it does not play */
		}
		closing = false;
		if (!enabled) {
			idle = false;
			enabled = true;
			audio_set_source(theremin_audio, NULL);
		}
	} else if (enabled) {
		closing = true; /* the voices fade; theremin_service() lets go after */
	}
}

void theremin_service(void)
{
	static uint32_t m_t, m_blocks;
	uint32_t blocks, errors, now = gfx_now_us();

	if (enabled && closing && idle) {
		audio_release_source(theremin_audio);
		enabled = false;
		closing = false;
	}
	/* the DAC's rate from its block count: it runs on its own PLL (47.7 kHz,
	 * not 48), and both the pitch and the analysis axis use what it really is */
	audio_get_counters(&blocks, &errors);
	if (m_t == 0 || blocks < m_blocks) {
		m_t = now;
		m_blocks = blocks;
	} else if (now - m_t >= 2000000u) {
		float r = (float)(blocks - m_blocks) * AUDIO_BLOCK_FRAMES * 1e6f / (float)(now - m_t);

		if (r > 40000.0f && r < 56000.0f) {
			rate = rate == (float)AUDIO_RATE ? r : rate + 0.3f * (r - rate);
		}
		m_t = now;
		m_blocks = blocks;
	}
}

void theremin_set_field(int x1, int y1, int x2, int y2, int play_x0, int play_x1)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	field.x1 = x1, field.y1 = y1, field.x2 = x2, field.y2 = y2;
	field.px0 = play_x0, field.px1 = play_x1;
	k_spin_unlock(&lock, key);
}

void theremin_set_wave(int w)
{
	wave = ((w % THM_WAVES) + THM_WAVES) % THM_WAVES;
}

const char *theremin_wave_name(int w)
{
	static const char *const names[] = {"SINE", "THEREMIN", "TRIANGLE", "SAWTOOTH"};

	return names[((w % THM_WAVES) + THM_WAVES) % THM_WAVES];
}

void theremin_set_snap(bool on)
{
	snap_on = on;
}

void theremin_set_sound(bool on)
{
	sound_on = on;
}

void theremin_ghost(bool on)
{
	if (on && !ghost_on) {
		ghost_n = 0;
	}
	ghost_on = on;
}

void theremin_remote(int v, float hz, float db)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (v < 0) {
		remote_on = false;
		memset(remote_t, 0, sizeof(remote_t));
	} else if (v < THM_VOICES) {
		remote_on = true;
		remote_t[v].on = hz > 0.0f;
		remote_t[v].hz = CLAMP(hz, 10.0f, 22000.0f);
		remote_t[v].db = CLAMP(db, SIG_DB_MIN, THM_TOP_DB);
		remote_t[v].vib = 0.0f;
		remote_t[v].t_us = 0;
	}
	k_spin_unlock(&lock, key);
}

void theremin_get(struct thm_status *st)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*st = pub;
	k_spin_unlock(&lock, key);
	st->enabled = enabled;
	st->mode = remote_on ? THM_REMOTE : ghost_on ? THM_GHOST : THM_TOUCH;
	st->wave = wave;
	st->snap = snap_on;
	st->sound = sound_on;
	st->rate_hz = rate;
}

float theremin_rate(void)
{
	return rate;
}

uint32_t theremin_total(void)
{
	return ring_w;
}

uint32_t theremin_copy(uint32_t from, int16_t *dst, uint32_t n)
{
	uint32_t end = ring_w;

	if (ring == NULL || end - from > RING_N) {
		return 0;
	}
	n = MIN(n, end - from);
	for (uint32_t i = 0; i < n; i++) {
		dst[i] = ring[(from + i) & (RING_N - 1)];
	}
	return n;
}

void theremin_note(float hz, char *buf, size_t len)
{
	static const char *const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	float m = 69.0f + 12.0f * log2f(MAX(hz, 8.0f) / 440.0f);
	int k = (int)roundf(m), cents = (int)roundf((m - (float)k) * 100.0f);

	if (cents > -3 && cents < 3) {
		snprintf(buf, len, "%s%d", names[k % 12], k / 12 - 1);
	} else {
		snprintf(buf, len, "%s%d %+d", names[k % 12], k / 12 - 1, cents);
	}
}
