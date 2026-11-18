/*
 * Signal generator and spectrum analysis. See sig.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <arm_math.h>

#include "platform/gfx.h"
#include "platform/mic.h"
#include "platform/psram.h"
#include "monitor.h"
#include "sig.h"
#include "theremin.h"

#define RING        4096              /* samples, power of two */
#define F_LO        20.0f
#define F_HI        20000.0f
#define TWO_PI      6.28318530718f

/* The sample rate the analysis assumes: nominal for the generator (it makes
 * its own samples at 48 kHz), the DAC's measured rate for the theremin and
 * the microphone (which runs from the DAC's clock: 47.7 kHz, not 48). */
static float fs_now = SIG_FS;
#define BIN_HZ (fs_now / SIG_FFT)

static int16_t ring[RING];
static uint32_t ring_w;               /* total samples written */
static int16_t scope[SIG_SCOPE];

static float win[SIG_FFT];
static float fft_in[SIG_FFT], fft_out[SIG_FFT];
static float bin_db[SIG_FFT / 2];
static float col_bin[SIG_COLS + 1];   /* fractional FFT bin at each column edge */
static float col_db[SIG_COLS], col_peak[SIG_COLS];
static uint8_t *waterfall;
static arm_rfft_fast_instance_f32 rfft;
static struct sig_view view;
static uint32_t last_ms;
static int source = SIG_SRC_GENERATOR;
static uint32_t mic_at;                /* next microphone sample to take */

/* ---- generator ----------------------------------------------------------
 * A deliberately busy test signal: a steady tone with harmonics and tremolo,
 * a slow logarithmic sweep, a gated FM warble and a noise floor.
 */
static float ph_a, ph_trem, ph_sweep, ph_fm, ph_fmmod;
static float sweep_pos;               /* 0..2: up then down, in log frequency */
static uint32_t rng = 0x1234abcd;
static uint32_t gen_n;

static inline float noise(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (float)(int32_t)rng * (1.0f / 2147483648.0f);
}

static inline float wrap(float ph)
{
	return ph >= 1.0f ? ph - 1.0f : ph;
}

static void generate(int n)
{
	const float dt = 1.0f / SIG_FS;

	for (int i = 0; i < n; i++) {
		float trem = 0.75f + 0.25f * arm_sin_f32(TWO_PI * ph_trem);
		float a = arm_sin_f32(TWO_PI * ph_a);
		float tone = trem * (0.25f * a + 0.03f * arm_sin_f32(TWO_PI * wrap(ph_a * 2.0f - floorf(ph_a * 2.0f))) +
				     0.015f * arm_sin_f32(TWO_PI * (ph_a * 3.0f - floorf(ph_a * 3.0f))));

		float u = sweep_pos < 1.0f ? sweep_pos : 2.0f - sweep_pos;
		float f_sweep = 80.0f * powf(150.0f, u);            /* 80 Hz .. 12 kHz */
		float sweep = 0.125f * arm_sin_f32(TWO_PI * ph_sweep);

		float gate = ((gen_n / (SIG_FS / 2)) & 3) < 2 ? 1.0f : 0.0f;
		float f_fm = 3000.0f + 800.0f * arm_sin_f32(TWO_PI * ph_fmmod);
		float fm = gate * 0.03f * arm_sin_f32(TWO_PI * ph_fm);

		float s = tone + sweep + fm + 0.0005f * noise();

		ring[ring_w & (RING - 1)] = (int16_t)(CLAMP(s, -1.0f, 1.0f) * 32767.0f);
		ring_w++;
		gen_n++;

		ph_a = wrap(ph_a + 440.0f * dt);
		ph_trem = wrap(ph_trem + 0.3f * dt);
		ph_sweep = wrap(ph_sweep + f_sweep * dt);
		ph_fm = wrap(ph_fm + f_fm * dt);
		ph_fmmod = wrap(ph_fmmod + 5.0f * dt);
		sweep_pos += dt / 6.0f;
		if (sweep_pos >= 2.0f) {
			sweep_pos -= 2.0f;
		}
	}
}

/* ---- analysis ----------------------------------------------------------- */

float sig_col_hz(int col)
{
	return F_LO * powf(F_HI / F_LO, (float)col / (SIG_COLS - 1));
}

int sig_hz_col(float hz)
{
	float c = logf(hz / F_LO) / logf(F_HI / F_LO) * (SIG_COLS - 1);

	return (int)CLAMP(c, 0.0f, (float)(SIG_COLS - 1));
}

float sig_colf_hz(float col)
{
	return F_LO * powf(F_HI / F_LO, col / (SIG_COLS - 1));
}

float sig_hz_colf(float hz)
{
	return logf(MAX(hz, 1.0f) / F_LO) / logf(F_HI / F_LO) * (SIG_COLS - 1);
}

/* Column edges in FFT bins for the rate the samples really have. */
static void set_rate(float fs)
{
	fs_now = fs;
	for (int c = 0; c <= SIG_COLS; c++) {
		col_bin[c] = F_LO * powf(F_HI / F_LO, ((float)c - 0.5f) / (SIG_COLS - 1)) / BIN_HZ;
	}
}

void sig_init(void)
{
	for (int i = 0; i < SIG_FFT; i++) {
		win[i] = 0.5f - 0.5f * cosf(TWO_PI * i / SIG_FFT);
	}
	set_rate(SIG_FS);
	theremin_init();
	monitor_init();
	for (int c = 0; c < SIG_COLS; c++) {
		col_db[c] = col_peak[c] = SIG_DB_MIN;
	}
	arm_rfft_fast_init_f32(&rfft, SIG_FFT);
	waterfall = psram_alloc(SIG_COLS * SIG_WF_H, 32);
	if (waterfall) {
		memset(waterfall, 0, SIG_COLS * SIG_WF_H);
	}
	view.col_db = col_db;
	view.col_peak = col_peak;
	view.scope = scope;
	view.waterfall = waterfall;
	view.source = "SIGNAL GENERATOR";
}

/* ---- microphone ---------------------------------------------------------
 * The samples are 24 bit; the ring here is 16. This microphone is a quiet one
 * (94 dB SPL is 41 dB under its full scale, which it reaches at 135 dB SPL),
 * so the top bits are empty in a room: take bits 18..3, which is +30 dB and
 * puts full scale at about 105 dB SPL.
 */
#define MIC_SHIFT 3

static bool take_microphone(void)
{
	static int32_t tmp[256];
	uint32_t end = mic_total();
	bool any = false;

	if (end - mic_at > RING / 2) {
		mic_at = end - RING / 2; /* after a pause: only the latest */
	}
	while (mic_at != end) {
		uint32_t n = mic_copy(mic_at, tmp, MIN(end - mic_at, (uint32_t)ARRAY_SIZE(tmp)));

		if (n == 0) {
			mic_at = end;
			break;
		}
		for (uint32_t i = 0; i < n; i++) {
			ring[ring_w++ & (RING - 1)] = (int16_t)CLAMP(tmp[i] >> MIC_SHIFT, -32768, 32767);
		}
		mic_at += n;
		any = true;
	}
	return any;
}

/* ---- the microphone monitor: what it sends to the headphones ---------------
 * While it runs (Signal Lab showing with MICROPHONE), the analysis reads its
 * output, gain and equalizer included, so the spectrum shows what is heard.
 */
static uint32_t mon_at;
static bool from_monitor;

static bool take_monitor(void)
{
	static int16_t tmp[256];
	uint32_t end = monitor_total();
	bool any = false;

	if (end - mon_at > RING / 2) {
		mon_at = end - RING / 2;
	}
	while (mon_at != end) {
		uint32_t n = monitor_copy(mon_at, tmp, MIN(end - mon_at, (uint32_t)ARRAY_SIZE(tmp)));

		if (n == 0) {
			mon_at = end;
			break;
		}
		for (uint32_t i = 0; i < n; i++) {
			ring[ring_w++ & (RING - 1)] = tmp[i];
		}
		mon_at += n;
		any = true;
	}
	return any;
}

/* The source's caption: the gain is the monitor's while it runs, the fixed
 * +30 dB of take_microphone() otherwise. */
static const char *mic_caption(void)
{
	static const char *const cap[] = {"MICROPHONE  +20 dB", "MICROPHONE  +30 dB", "MICROPHONE  +40 dB"};
	struct mon_status st;

	if (!from_monitor) {
		return cap[1];
	}
	monitor_get(&st);
	return st.gain_db <= 20 ? cap[0] : st.gain_db >= 40 ? cap[2] : cap[1];
}

/* ---- theremin: what it sent to the DAC ------------------------------------ */
static uint32_t thm_at;

static bool take_theremin(void)
{
	static int16_t tmp[256];
	uint32_t end = theremin_total();
	bool any = false;

	if (end - thm_at > RING / 2) {
		thm_at = end - RING / 2; /* after a pause: only the latest */
	}
	while (thm_at != end) {
		uint32_t n = theremin_copy(thm_at, tmp, MIN(end - thm_at, (uint32_t)ARRAY_SIZE(tmp)));

		if (n == 0) {
			thm_at = end;
			break;
		}
		for (uint32_t i = 0; i < n; i++) {
			ring[ring_w++ & (RING - 1)] = tmp[i];
		}
		thm_at += n;
		any = true;
	}
	return any;
}

void sig_set_source(int src)
{
	if (src == SIG_SRC_MIC && !mic_ok()) {
		src = SIG_SRC_GENERATOR;
	}
	source = src;
	mic_at = mic_total();
	thm_at = theremin_total();
	mon_at = monitor_total();
	from_monitor = false;
	set_rate(src == SIG_SRC_GENERATOR ? SIG_FS : theremin_rate());
	view.source = src == SIG_SRC_MIC        ? mic_caption()
		      : src == SIG_SRC_THEREMIN ? "THEREMIN  TOUCH TO PLAY"
						: "SIGNAL GENERATOR";
	for (int c = 0; c < SIG_COLS; c++) {
		col_peak[c] = SIG_DB_MIN; /* the other source's peaks mean nothing here */
	}
}

int sig_source(void)
{
	return source;
}

static void analyse(void)
{
	uint32_t t0 = gfx_now_us();
	uint32_t start = ring_w - SIG_FFT;
	float sum_sq = 0.0f;

	for (int i = 0; i < SIG_FFT; i++) {
		float s = ring[(start + i) & (RING - 1)] * (1.0f / 32768.0f);

		sum_sq += s * s;
		fft_in[i] = s * win[i];
	}
	arm_rfft_fast_f32(&rfft, fft_in, fft_out, 0);

	/* A full-scale sine through a Hann window peaks at N/4, so that is 0 dBFS. */
	const float norm = 1.0f / ((SIG_FFT / 4.0f) * (SIG_FFT / 4.0f));
	int peak_bin = 1;
	float peak_pow = 0.0f;

	bin_db[0] = SIG_DB_MIN;
	for (int k = 1; k < SIG_FFT / 2; k++) {
		float re = fft_out[2 * k], im = fft_out[2 * k + 1];
		float p = (re * re + im * im) * norm;

		if (p > peak_pow) {
			peak_pow = p;
			peak_bin = k;
		}
		bin_db[k] = p > 1e-10f ? 10.0f * log10f(p) : SIG_DB_MIN;
	}
	view.fft_us = gfx_now_us() - t0;

	/* parabolic interpolation around the peak bin for a sub-bin frequency */
	float d = 0.0f;

	if (peak_bin > 1 && peak_bin < SIG_FFT / 2 - 1) {
		float a = bin_db[peak_bin - 1], b = bin_db[peak_bin], c = bin_db[peak_bin + 1];
		float den = a - 2.0f * b + c;

		d = den != 0.0f ? 0.5f * (a - c) / den : 0.0f;
	}
	view.peak_hz = (peak_bin + d) * BIN_HZ;
	view.peak_db = bin_db[peak_bin];
	view.rms_db = sum_sq > 0.0f ? 10.0f * log10f(sum_sq / SIG_FFT * 2.0f) : SIG_DB_MIN;

	/* bins to log-spaced columns: interpolate where a column is narrower than
	 * a bin, take the maximum where it is wider
	 */
	view.wf_head = (view.wf_head + SIG_WF_H - 1) % SIG_WF_H;
	uint8_t *row = waterfall ? waterfall + view.wf_head * SIG_COLS : NULL;

	for (int c = 0; c < SIG_COLS; c++) {
		float b0 = col_bin[c], b1 = col_bin[c + 1];
		float v;

		if (b1 - b0 < 1.0f) {
			float mid = 0.5f * (b0 + b1);
			int k = (int)mid;
			float f = mid - k;

			k = CLAMP(k, 0, SIG_FFT / 2 - 2);
			v = bin_db[k] + f * (bin_db[k + 1] - bin_db[k]);
		} else {
			int k0 = (int)b0, k1 = MIN((int)b1, SIG_FFT / 2 - 1);

			v = bin_db[k0];
			for (int k = k0 + 1; k <= k1; k++) {
				v = MAX(v, bin_db[k]);
			}
		}
		v = MAX(v, SIG_DB_MIN);
		/* fast attack, slower release reads better than a raw spectrum */
		col_db[c] = v > col_db[c] ? v : col_db[c] + 0.22f * (v - col_db[c]);
		col_peak[c] = v > col_peak[c] ? v : MAX(col_peak[c] - 0.18f, v);
		if (row) {
			row[c] = (uint8_t)((v - SIG_DB_MIN) * (255.0f / -SIG_DB_MIN));
		}
	}

	/* scope: start at the most recent rising zero crossing that leaves a full window */
	uint32_t s0 = ring_w - SIG_SCOPE - 1;

	for (int back = 0; back < 1024; back++) {
		uint32_t i = ring_w - SIG_SCOPE - 2 - back;

		if (ring[i & (RING - 1)] < 0 && ring[(i + 1) & (RING - 1)] >= 0) {
			s0 = i + 1;
			break;
		}
	}
	for (int i = 0; i < SIG_SCOPE; i++) {
		scope[i] = ring[(s0 + i) & (RING - 1)];
	}
	view.frames++;
}

static bool held;

void sig_hold(bool hold)
{
	held = hold;
}

bool sig_held(void)
{
	return held;
}

void sig_service(uint32_t now_ms)
{
	uint32_t dt = last_ms ? now_ms - last_ms : 17;

	last_ms = now_ms;
	if (held) {
		return;
	}
	/* After a stall, skip ahead rather than generate seconds of samples. */
	int n = (int)(MIN(dt, 50u) * SIG_FS / 1000);

	if (n <= 0) {
		return;
	}
	if (source == SIG_SRC_MIC) {
		bool m = monitor_running();

		if (m != from_monitor) {
			/* the monitor started or stopped: read from the other one, from now */
			from_monitor = m;
			mon_at = monitor_total();
			mic_at = mic_total();
		}
		view.source = mic_caption();
		if (fabsf(theremin_rate() - fs_now) > 1.0f) {
			set_rate(theremin_rate());
		}
		if (!(m ? take_monitor() : take_microphone())) {
			return;
		}
	} else if (source == SIG_SRC_THEREMIN) {
		if (fabsf(theremin_rate() - fs_now) > 1.0f) {
			set_rate(theremin_rate()); /* the measurement settles in the first seconds */
		}
		if (!take_theremin()) {
			return;
		}
	} else {
		generate(n);
	}
	if (ring_w >= RING) {
		analyse();
	}
}

const struct sig_view *sig_view(void)
{
	return &view;
}
