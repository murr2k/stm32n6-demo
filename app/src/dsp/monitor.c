/*
 * Microphone monitor. See monitor.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <arm_math.h>

#include "monitor.h"
#include "platform/audio.h"
#include "platform/gfx.h"
#include "platform/mic.h"
#include "platform/psram.h"
#include "sig.h"
#include "theremin.h"

#define RING_N  8192 /* analysis ring, samples, power of two */
#define CHUNK   32   /* the bands are eased per chunk (0.67 ms) */
/* Microphone samples behind the newest when a block starts. The microphone's
 * thread hands over MIC_BLOCK at a time and the DAC asks for
 * AUDIO_BLOCK_FRAMES; the two come from one clock, so the distance stays
 * where it was set (measured: the same on every block). One block and 32
 * samples to start with, plus the DAC's own queue of one to two blocks.
 * Should the order in which the two threads run ever change from one block
 * to the next, the data runs short once, the monitor resyncs (a 5 ms
 * fade-in) and keeps a block more from then on. */
#define LAG_START (MAX(MIC_BLOCK, AUDIO_BLOCK_FRAMES) + 32)
#define LAG_STEP  MIC_BLOCK
#define LAG_LIMIT (LAG_START + 4 * LAG_STEP)
#define TWO_PI    6.283185307179586

/* The filters that run: Andrew Simper's trapezoidal state-variable filter
 * ("Linear Trap Optimised 2", Cytomic, 2013), in single precision. It is
 * well behaved in float down to 20 Hz, where a float biquad's coefficients
 * are too coarse, and its state stays meaningful while a band moves. Its
 * bell and shelves are the bilinear transforms, prewarped at the band's
 * frequency, of the same analogue prototypes as the Audio EQ Cookbook's
 * peaking and shelving biquads, so the curve (drawn from the cookbook's
 * coefficients, in double) is the response that plays. Double precision per
 * sample cost 0.75 ms a block on the audio thread (the M55's double unit is
 * slow for long dependent chains); this costs a fraction of that. */
struct svf {
	float a1, a2, a3, m0, m1, m2;
	float ic1, ic2;
};

/* A band as the audio thread has it: eased towards what is set, in log
 * frequency, dB and log Q. */
struct live {
	float lf, db, lq;
};

static const struct mon_band start_band[MON_BANDS] = {
	{100.0f, 0.0f, 0.71f}, {400.0f, 0.0f, 1.0f}, {1500.0f, 0.0f, 1.0f}, {5000.0f, 0.0f, 1.0f}, {10000.0f, 0.0f, 0.71f},
};

static struct k_spinlock lock;
static struct mon_band set_band[MON_BANDS]; /* under lock */
static uint32_t version;                     /* under lock */
static struct mon_status pub;                /* the audio thread's figures, under lock */
static volatile bool enabled, closing, listen_on, eq_on = true, fresh;
static volatile int gain_db = 30;
static volatile float tone_hz, tone_db = -20.0f;
static uint32_t closing_us;

/* audio thread only (or with the monitor not the source) */
static struct svf sv[MON_BANDS];
static struct live lv[MON_BANDS];
static bool synced;
static uint32_t rd, lag_set = LAG_START;
static float in_gain, mix, sync_ramp, env, tone_amp, tone_ph, duck, pre_pow, post_pow;
static volatile float out_gain;
static uint32_t n_blocks, n_resyncs, n_limited, lag_min = UINT32_MAX, lag_max;
static int16_t *ring;
static volatile uint32_t ring_w;

static void col_table(float fs);

int monitor_band_type(int i)
{
	return i == 0 ? MON_LOW_SHELF : i == MON_BANDS - 1 ? MON_HIGH_SHELF : MON_PEAK;
}

const char *monitor_band_name(int i)
{
	static const char *const names[MON_BANDS] = {"LOW SHELF", "PEAK 2", "PEAK 3", "PEAK 4", "HIGH SHELF"};

	return names[CLAMP(i, 0, MON_BANDS - 1)];
}

static float clamp_hz(float hz, double fs)
{
	return CLAMP(hz, MON_HZ_MIN, MIN(MON_HZ_MAX, (float)(0.45 * fs)));
}

/* Shelves above Q 0.71 overshoot (a bump before the shelf), which a finger
 * may want, but not without end. */
static float clamp_q(int i, float q)
{
	return monitor_band_type(i) == MON_PEAK ? CLAMP(q, MON_Q_MIN, MON_Q_MAX) : CLAMP(q, MON_Q_MIN, 2.0f);
}

static void svf_design(struct svf *f, int type, float hz, float db, float q, float fs)
{
	float A = powf(10.0f, db / 40.0f), g = tanf(3.14159265f * hz / fs), k;

	if (type == MON_PEAK) {
		k = 1.0f / (q * A);
		f->m0 = 1.0f;
		f->m1 = k * (A * A - 1.0f);
		f->m2 = 0.0f;
	} else if (type == MON_LOW_SHELF) {
		g /= sqrtf(A);
		k = 1.0f / q;
		f->m0 = 1.0f;
		f->m1 = k * (A - 1.0f);
		f->m2 = A * A - 1.0f;
	} else {
		g *= sqrtf(A);
		k = 1.0f / q;
		f->m0 = A * A;
		f->m1 = k * (1.0f - A) * A;
		f->m2 = 1.0f - A * A;
	}
	f->a1 = 1.0f / (1.0f + g * (g + k));
	f->a2 = g * f->a1;
	f->a3 = g * f->a2;
}

static inline float svf_run(struct svf *f, float v0)
{
	float v3 = v0 - f->ic2;
	float v1 = f->a1 * f->ic1 + f->a2 * v3;
	float v2 = f->ic2 + f->a2 * f->ic1 + f->a3 * v3;

	f->ic1 = 2.0f * v1 - f->ic1;
	f->ic2 = 2.0f * v2 - f->ic2;
	return f->m0 * v0 + f->m1 * v1 + f->m2 * v2;
}

/* The same bands as cookbook biquads, normalised to a0 = 1, for the curve. */
struct biquad {
	double b0, b1, b2, a1, a2;
};

static void design(struct biquad *f, int type, double hz, double db, double q, double fs)
{
	double A = pow(10.0, db / 40.0), w0 = TWO_PI * hz / fs;
	double cw = cos(w0), alpha = sin(w0) / (2.0 * q);
	double b0, b1, b2, a0, a1, a2;

	if (type == MON_PEAK) {
		b0 = 1.0 + alpha * A;
		b1 = -2.0 * cw;
		b2 = 1.0 - alpha * A;
		a0 = 1.0 + alpha / A;
		a1 = -2.0 * cw;
		a2 = 1.0 - alpha / A;
	} else {
		double sa = 2.0 * sqrt(A) * alpha, p = A + 1.0, m = A - 1.0;

		if (type == MON_LOW_SHELF) {
			b0 = A * (p - m * cw + sa);
			b1 = 2.0 * A * (m - p * cw);
			b2 = A * (p - m * cw - sa);
			a0 = p + m * cw + sa;
			a1 = -2.0 * (m + p * cw);
			a2 = p + m * cw - sa;
		} else {
			b0 = A * (p + m * cw + sa);
			b1 = -2.0 * A * (m + p * cw);
			b2 = A * (p + m * cw - sa);
			a0 = p - m * cw + sa;
			a1 = 2.0 * (m - p * cw);
			a2 = p - m * cw - sa;
		}
	}
	f->b0 = b0 / a0;
	f->b1 = b1 / a0;
	f->b2 = b2 / a0;
	f->a1 = a1 / a0;
	f->a2 = a2 / a0;
}

/* What is set, in the units the bands are eased in. Once per block. */
static void targets(const struct mon_band *tb, double fs, struct live *t)
{
	for (int b = 0; b < MON_BANDS; b++) {
		t[b].lf = log2f(clamp_hz(tb[b].hz, fs));
		t[b].db = CLAMP(tb[b].db, -MON_DB_MAX, MON_DB_MAX);
		t[b].lq = log2f(clamp_q(b, tb[b].q));
	}
}

/* Ease the bands one chunk towards what is set and redesign those that
 * moved (all of them when the DAC's measured rate has moved). */
static void ease_bands(const struct live *t, float fs, float k)
{
	static float fs_design;
	bool all = fresh || fabsf(fs - fs_design) > 1.0f;

	for (int b = 0; b < MON_BANDS; b++) {
		struct live *L = &lv[b];
		float d_lf = t[b].lf - L->lf, d_db = t[b].db - L->db, d_lq = t[b].lq - L->lq;

		if (fresh) {
			*L = t[b];
		} else if (fabsf(d_lf) < 1e-4f && fabsf(d_db) < 1e-3f && fabsf(d_lq) < 1e-4f) {
			if (d_lf == 0.0f && d_db == 0.0f && d_lq == 0.0f && !all) {
				continue; /* settled, coefficients current */
			}
			*L = t[b]; /* the last step: exactly there */
		} else {
			L->lf += k * d_lf;
			L->db += k * d_db;
			L->lq += k * d_lq;
		}
		svf_design(&sv[b], monitor_band_type(b), exp2f(L->lf), L->db, exp2f(L->lq), fs);
	}
	fs_design = fs;
	fresh = false;
}

/* Goertzel's power at the test tone over n samples, as the dBFS of a sine. */
static float goertzel_db(float p, int n)
{
	return p > 1e-20f ? 10.0f * log10f(4.0f * p / ((float)n * (float)n)) : -200.0f;
}

static uint32_t monitor_audio(int16_t *dst, uint32_t frames, void *ctx)
{
	ARG_UNUSED(ctx);
	struct mon_band tb[MON_BANDS];
	struct live tgt[MON_BANDS];
	int32_t buf[CHUNK];
	uint32_t c0 = k_cycle_get_32();
	int n = (int)MIN(frames, (uint32_t)AUDIO_BLOCK_FRAMES);
	float fs = theremin_rate(); /* the DAC's measured rate; the microphone shares its clock */
	k_spinlock_key_t key = k_spin_lock(&lock);

	memcpy(tb, set_band, sizeof(tb));
	k_spin_unlock(&lock, key);
	targets(tb, fs, tgt);

	uint32_t end = mic_total(), lag = end - rd;

	if (!synced || lag < (uint32_t)n || lag > lag_set + 3 * LAG_STEP) {
		/* start, or the data ran short or far ahead: from lag_set behind the
		 * newest, faded in from silence (the jump itself is not heard) */
		if (synced) {
			n_resyncs++;
			if (lag < (uint32_t)n) {
				lag_set = MIN(lag_set + LAG_STEP, (uint32_t)LAG_LIMIT);
			}
		}
		synced = true;
		rd = end - lag_set;
		lag = lag_set;
		sync_ramp = 0.0f;
	}
	lag_min = MIN(lag_min, lag);
	lag_max = MAX(lag_max, lag);

	/* time constants: bands 20 ms, gain and the EQ switch 10 ms, LISTEN 10 ms
	 * (4 ms on the way out), a resync's fade-in and the test tone 5 ms, the
	 * limiter's release 100 ms (its attack is instant: the ceiling is never
	 * passed) */
	float k_band = 1.0f - expf(-(float)CHUNK / (0.020f * fs));
	float k10 = 1.0f - expf(-1.0f / (0.010f * fs)), k_out = closing ? 1.0f - expf(-1.0f / (0.004f * fs)) : k10;
	float k5 = 1.0f - expf(-1.0f / (0.005f * fs)), rel = expf(-1.0f / (0.100f * fs));
	float ceil = powf(10.0f, MON_CEIL_DB / 20.0f);
	float gain_t = powf(10.0f, (float)gain_db / 20.0f) / 8388608.0f; /* 24-bit full scale is 1 */
	float out_t = listen_on && !closing ? 1.0f : 0.0f, mix_t = eq_on ? 1.0f : 0.0f, og = out_gain;
	/* the test tone: the DAC gets it instead of the microphone, and Goertzel's
	 * filter measures it in the microphone before and after the equalizer */
	float thz = tone_hz, tone_t = thz > 0.0f && !closing ? powf(10.0f, tone_db / 20.0f) : 0.0f;
	float t_inc = thz / fs, gz_c = 2.0f * cosf(6.2831853f * t_inc);
	float p1 = 0.0f, p2 = 0.0f, q1 = 0.0f, q2 = 0.0f;
	bool limited = false;
	float least = 1.0f;

	for (int c = 0; c < n; c += CHUNK) {
		int m = MIN(CHUNK, n - c);
		uint32_t got = mic_copy(rd, buf, (uint32_t)m);

		for (int i = (int)got; i < m; i++) {
			buf[i] = 0;
		}
		rd += (uint32_t)m;
		ease_bands(tgt, fs, k_band);

		for (int i = 0; i < m; i++) {
			in_gain += k10 * (gain_t - in_gain);
			float x = (float)buf[i] * in_gain, y = x;

			for (int b = 0; b < MON_BANDS; b++) {
				y = svf_run(&sv[b], y);
			}
			/* the filters always run, so switching the EQ is a cross-fade */
			mix += k10 * (mix_t - mix);
			float s = (x + mix * (y - x)) * sync_ramp;

			sync_ramp += k5 * (1.0f - sync_ramp);
			if (thz > 0.0f) {
				float g0 = x + gz_c * p1 - p2, h0 = s + gz_c * q1 - q2;

				p2 = p1, p1 = g0;
				q2 = q1, q1 = h0;
			}

			float a = fabsf(s);

			env = a > env ? a : env * rel;
			if (env > ceil) {
				float g = ceil / env;

				s *= g;
				least = MIN(least, g);
				limited = true;
			}
			og += k_out * (out_t - og);
			tone_amp += k5 * (tone_t - tone_amp);
			duck += k5 * ((tone_t > 0.0f ? 1.0f : 0.0f) - duck);

			float o = s * og * (1.0f - duck);

			if (tone_amp > 1e-6f) {
				o += tone_amp * arm_sin_f32(6.2831853f * tone_ph);
				tone_ph += t_inc;
				tone_ph -= (float)(int)tone_ph;
			}
			int16_t q = (int16_t)(s * 32767.0f);

			dst[2 * (c + i)] = dst[2 * (c + i) + 1] = (int16_t)(CLAMP(o, -1.0f, 1.0f) * 32767.0f);
			if (ring) {
				ring[(ring_w + (uint32_t)(c + i)) & (RING_N - 1)] = q;
			}
		}
	}
	if (fabsf(out_t - og) < 1e-4f) {
		og = out_t; /* -80 dB from the end: exactly there */
	}
	out_gain = og;
	ring_w += (uint32_t)n;
	n_blocks++;
	n_limited += limited;
	if (thz > 0.0f) {
		/* power per block, averaged over about eight blocks */
		float pp = p1 * p1 + p2 * p2 - gz_c * p1 * p2, qp = q1 * q1 + q2 * q2 - gz_c * q1 * q2;

		pre_pow += 0.125f * (pp - pre_pow);
		post_pow += 0.125f * (qp - post_pow);
	}

	uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);

	key = k_spin_lock(&lock);
	pub.blocks = n_blocks;
	pub.resyncs = n_resyncs;
	pub.lag = lag;
	pub.lag_min = lag_min;
	pub.lag_max = lag_max;
	pub.delay_us = (uint32_t)((float)(lag + audio_queued_frames()) * 1e6f / fs);
	pub.dsp_us = us;
	pub.dsp_worst_us = MAX(pub.dsp_worst_us, us);
	pub.reduce_db = 20.0f * log10f(least);
	pub.limited = n_limited;
	pub.tone_hz = thz;
	pub.tone_pre_db = thz > 0.0f ? goertzel_db(pre_pow, n) : -200.0f;
	pub.tone_post_db = thz > 0.0f ? goertzel_db(post_pow, n) : -200.0f;
	k_spin_unlock(&lock, key);
	return (uint32_t)n;
}

/* ---- control ----------------------------------------------------------------- */

void monitor_init(void)
{
	memcpy(set_band, start_band, sizeof(set_band));
	col_table(theremin_rate()); /* nominal now; remade once the rate is measured */
	ring = psram_alloc(RING_N * sizeof(int16_t), 32);
	if (ring) {
		memset(ring, 0, RING_N * sizeof(int16_t));
	}
}

void monitor_enable(bool on)
{
	if (on) {
		closing = false;
		if (!enabled) {
			/* not the source now, so the audio thread is not in here */
			synced = false;
			fresh = true;
			out_gain = 0.0f;
			sync_ramp = 0.0f;
			env = 0.0f;
			tone_amp = duck = 0.0f;
			in_gain = powf(10.0f, (float)gain_db / 20.0f) / 8388608.0f;
			mix = eq_on ? 1.0f : 0.0f;
			memset(sv, 0, sizeof(sv));
			enabled = true;
			audio_set_source(monitor_audio, NULL);
		}
	} else if (enabled && !closing) {
		closing = true; /* the output fades; monitor_service() lets go after */
		closing_us = gfx_now_us();
	}
}

void monitor_service(void)
{
	/* faded to -60 dB (the tone too), or another source took the output
	 * meanwhile and the fade never ran (then the timeout) */
	if (enabled && closing && ((out_gain < 1e-3f && tone_amp < 1e-3f) || gfx_now_us() - closing_us > 150000u)) {
		audio_release_source(monitor_audio);
		enabled = false;
		closing = false;
	}
}

bool monitor_running(void)
{
	return enabled && !closing;
}

bool monitor_enabled(void)
{
	return enabled;
}

static void changed(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	version++;
	k_spin_unlock(&lock, key);
}

void monitor_set_listen(bool on)
{
	listen_on = on;
	changed();
}

void monitor_set_gain(int db)
{
	gain_db = CLAMP(db, 0, 50);
	changed();
}

void monitor_set_eq(bool on)
{
	eq_on = on;
	changed();
}

void monitor_tone(float hz, float db)
{
	tone_db = CLAMP(db, -60.0f, -6.0f);
	tone_hz = hz > 0.0f ? CLAMP(hz, 20.0f, 20000.0f) : 0.0f;
	changed();
}

void monitor_set_band(int i, float hz, float db, float q)
{
	if (i < 0 || i >= MON_BANDS) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);

	set_band[i].hz = CLAMP(hz, MON_HZ_MIN, MON_HZ_MAX);
	set_band[i].db = CLAMP(db, -MON_DB_MAX, MON_DB_MAX);
	set_band[i].q = clamp_q(i, q);
	version++;
	k_spin_unlock(&lock, key);
}

void monitor_flat(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	for (int i = 0; i < MON_BANDS; i++) {
		set_band[i].db = 0.0f;
	}
	version++;
	k_spin_unlock(&lock, key);
}

void monitor_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	memcpy(set_band, start_band, sizeof(set_band));
	version++;
	k_spin_unlock(&lock, key);
}

void monitor_get(struct mon_status *st)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*st = pub;
	memcpy(st->band, set_band, sizeof(st->band));
	st->version = version;
	k_spin_unlock(&lock, key);
	st->enabled = enabled;
	st->listen = listen_on;
	st->eq = eq_on;
	st->gain_db = gain_db;
	st->tone_db = tone_db;
}

/* |H(e^jw)|^2 of a biquad: (B0 + B1 cos w + B2 cos 2w) / (A0 + A1 cos w + A2 cos 2w),
 * with the B and A terms made once per band. */
struct resp {
	double nb[MON_BANDS][3], na[MON_BANDS][3];
};

static void resp_init(struct resp *r, const struct mon_status *st, double fs)
{
	for (int b = 0; b < MON_BANDS; b++) {
		struct biquad f;

		design(&f, monitor_band_type(b), clamp_hz(st->band[b].hz, fs), st->band[b].db,
		       clamp_q(b, st->band[b].q), fs);
		r->nb[b][0] = f.b0 * f.b0 + f.b1 * f.b1 + f.b2 * f.b2;
		r->nb[b][1] = 2.0 * (f.b0 * f.b1 + f.b1 * f.b2);
		r->nb[b][2] = 2.0 * f.b0 * f.b2;
		r->na[b][0] = 1.0 + f.a1 * f.a1 + f.a2 * f.a2;
		r->na[b][1] = 2.0 * (f.a1 + f.a1 * f.a2);
		r->na[b][2] = 2.0 * f.a2;
	}
}

static double resp_at(const struct resp *r, double cw)
{
	double c2w = 2.0 * cw * cw - 1.0, p = 1.0;

	for (int b = 0; b < MON_BANDS; b++) {
		p *= (r->nb[b][0] + r->nb[b][1] * cw + r->nb[b][2] * c2w) /
		     (r->na[b][0] + r->na[b][1] * cw + r->na[b][2] * c2w);
	}
	return p > 1e-12 ? 10.0 * log10(p) : -120.0;
}

/* The curve for the screen, every display column, in float. A cookbook
 * filter is the bilinear transform of an analogue prototype prewarped at its
 * frequency, so its magnitude at digital frequency w is the prototype's at
 * W = tan(w/2) / tan(w0/2):
 *   peak:       ((1-W^2)^2 + (A W/Q)^2) / ((1-W^2)^2 + (W/(A Q))^2)
 *   low shelf:  A^2 ((A-W^2)^2 + A W^2/Q^2) / ((1-A W^2)^2 + A W^2/Q^2)
 *   high shelf: A^2 ((1-A W^2)^2 + A W^2/Q^2) / ((A-W^2)^2 + A W^2/Q^2)
 * That is well conditioned in float where the expanded biquad form (as in
 * monitor_response_db) cancels, and cheap: done in double with a cos, a pow
 * and five divisions a column it took several ms of a frame. tan(w/2) per
 * column is a table, made again when the measured rate moves. */
static float *col_t;
static float col_t_fs;

/* tan(pi f / fs) at every column. The columns' frequencies are a geometric
 * series (sig_col_hz), so each is the last times a constant; sine and cosine
 * are the CMSIS tables. 0.1 ms, against 2 ms with the C library's powf and
 * tanf per column, which landed on Signal Lab's first frame. Remade when the
 * DAC's measured rate moves by more than 0.1 % (the curve's error from that
 * is far below a pixel). */
static void col_table(float fs)
{
	if (col_t == NULL) {
		col_t = psram_alloc(SIG_COLS * sizeof(float), 32);
		if (col_t == NULL) {
			return;
		}
	}
	double f = sig_col_hz(0), step = (double)sig_col_hz(SIG_COLS - 1) / f;

	step = pow(step, 1.0 / (SIG_COLS - 1));
	for (int c = 0; c < SIG_COLS; c++, f *= step) {
		float x = 3.14159265f * MIN((float)f, 0.49f * fs) / fs;

		col_t[c] = arm_sin_f32(x) / arm_cos_f32(x);
	}
	col_t_fs = fs;
}

void monitor_curve(const struct mon_status *st, int16_t *cdb, int n)
{
	float fs = theremin_rate();
	float iw[MON_BANDS], A[MON_BANDS], A2[MON_BANDS], kq[MON_BANDS], kq2[MON_BANDS];
	bool flat = true;

	n = MIN(n, SIG_COLS);
	for (int b = 0; b < MON_BANDS; b++) {
		flat = flat && st->band[b].db == 0.0f;
	}
	if (!flat && (col_t == NULL || fabsf(fs - col_t_fs) > 0.001f * fs)) {
		col_table(fs);
	}
	if (flat || col_t == NULL) {
		memset(cdb, 0, (size_t)n * sizeof(*cdb)); /* flat: 0 dB everywhere, nothing to work out */
		return;
	}
	for (int b = 0; b < MON_BANDS; b++) {
		float q = clamp_q(b, st->band[b].q);

		iw[b] = 1.0f / tanf(3.14159265f * clamp_hz(st->band[b].hz, fs) / fs);
		A[b] = powf(10.0f, CLAMP(st->band[b].db, -MON_DB_MAX, MON_DB_MAX) / 40.0f);
		A2[b] = A[b] * A[b];
		kq[b] = A[b] / (q * q);   /* shelves: A / Q^2 */
		kq2[b] = 1.0f / (q * q); /* peaks: 1 / Q^2 */
	}
	for (int c = 0; c < n; c++) {
		float p = 1.0f;

		for (int b = 0; b < MON_BANDS; b++) {
			float W = col_t[c] * iw[b], W2 = W * W;

			if (st->band[b].db == 0.0f) {
				continue; /* flat: exactly 1 */
			}
			int type = monitor_band_type(b);

			if (type == MON_PEAK) {
				float u = (1.0f - W2) * (1.0f - W2);

				p *= (u + A2[b] * W2 * kq2[b]) / (u + W2 * kq2[b] / A2[b]);
			} else {
				float lo = (A[b] - W2) * (A[b] - W2), hi = (1.0f - A[b] * W2) * (1.0f - A[b] * W2);
				float m = kq[b] * W2;

				p *= type == MON_LOW_SHELF ? A2[b] * (lo + m) / (hi + m) : A2[b] * (hi + m) / (lo + m);
			}
		}
		cdb[c] = (int16_t)CLAMP(1000.0f * log10f(MAX(p, 1e-12f)), -30000.0f, 30000.0f);
	}
}

float monitor_response_db(const struct mon_status *st, float hz)
{
	double fs = theremin_rate();
	struct resp r;

	resp_init(&r, st, fs);
	return (float)resp_at(&r, cos(TWO_PI * (double)hz / fs));
}

uint32_t monitor_total(void)
{
	return ring_w;
}

uint32_t monitor_copy(uint32_t from, int16_t *dst, uint32_t n)
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
